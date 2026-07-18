#include "ImageViewer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdRender/product.h>

#include "FileBrowser.h"
#include "FileImageSource.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ImageCache.h"
#include "ImageCompositor.h"
#include "ImageSequence.h"
#include "ImagingSettings.h"
#include "InfiniteCanvas.h"
#include "ModalDialogs.h"
#include "RenderImageSource.h"
#include "ViewportEngine.h"

// One image slot: which source of the store it shows and the last buffer
// resolved from the cache (kept displayed while the current frame loads)
struct ViewerSlot {
    ImageSourcePtr source;
    ImageBufferPtr image;

    bool HasValidImage() const { return image && image->IsValid(); }
};

// The viewer is unique by design: one canvas, two compared slots (A/B), a
// store of every source opened or dropped, and the frame cache, living for
// the whole application session.
struct ImageViewerState {
    InfiniteCanvas canvas;
    ViewerSlot slots[2];
    std::vector<ImageSourcePtr> store;
    ImageCache cache;
    // Session render setup driving new renders (phase authoring to the stage
    // comes later)
    RenderSetup renderSetup;
    bool renderSetupInitialized = false;
    int renderRange[2] = {1, 24};
    bool renderRangeInitialized = false;
    // compositors[0] renders A (and the combined wipe/difference modes),
    // compositors[1] renders B for the side-by-side mode
    ImageCompositor compositors[2];
    ImageCompositeParams params;
    bool linkedDisplay = true; // B follows A's exposure/gamma

    // Frame selection: locked on the editor timeline, or the viewer transport
    bool lockToTimeline = true;
    int transportFrame = 0;
    bool playing = false;
    double playbackAccumulator = 0.0;

    bool fitPending = false;
    bool draggingWipe = false;

    ImageViewerState() {
        // Inspecting pixels calls for a much wider zoom range than the node editor
        canvas.zoomMin = 1.f / 64.f;
        canvas.zoomMax = 64.f;
    }
};

static ImageViewerState viewer;

static void AssignSourceToSlot(int slot, const ImageSourcePtr &source) {
    viewer.slots[slot].source = source;
    viewer.slots[slot].image = nullptr;
    viewer.fitPending = true;
}

// Open a path in a slot: reuse the store source covering it or create one
static void OpenPathInSlot(int slot, const std::string &filePath) {
    ImageSourcePtr source = CreateImageSource(filePath);
    for (const auto &existing : viewer.store) {
        if (existing->identity == source->identity) {
            AssignSourceToSlot(slot, existing);
            return;
        }
    }
    viewer.store.push_back(source);
    AssignSourceToSlot(slot, source);
}

// Set when an open request wants the panel visible, consumed by the editor
static bool showRequested = false;

void ImageViewerOpenFile(const std::string &filePath, int slot) {
    OpenPathInSlot(slot == 0 ? 0 : 1, filePath);
    showRequested = true;
}

void ImageViewerOpenAsset(const std::string &assetPath) { ImageViewerOpenFile(FindFirstUdimTile(assetPath), 0); }

bool ImageViewerConsumeShowRequest() {
    const bool requested = showRequested;
    showRequested = false;
    return requested;
}

struct OpenImageModalDialog : public ModalDialog {
    OpenImageModalDialog(int slot) : slot(slot) { SetValidExtensions(GetImageFileExtensions()); }
    ~OpenImageModalDialog() override {}
    void Draw() override {
        DrawFileBrowser(RemainingHeight(2));
        if (!FilePathExists()) {
            ImGui::Text("Not found: ");
            ImGui::SameLine();
        }
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawModalButtonsOkCancel([&]() {
            if (!filePath.empty() && FilePathExists()) {
                OpenPathInSlot(slot, filePath);
            }
        });
    }
    const char *DialogId() const override { return slot == 0 ? "Open image A" : "Open image B"; }
    int slot = 0;
};

// Change the zoom while keeping the canvas point at the widget center in place
static void SetZoomCentered(InfiniteCanvas &canvas, float zoom) {
    const ImVec2 center(-canvas.scrolling.x / canvas.zooming, -canvas.scrolling.y / canvas.zooming);
    canvas.zooming = zoom;
    canvas.scrolling = ImVec2(-center.x * zoom, -center.y * zoom);
}

static void SwapSlots() {
    std::swap(viewer.slots[0], viewer.slots[1]);
    if (!viewer.linkedDisplay) std::swap(viewer.params.a, viewer.params.b);
    viewer.compositors[0].MarkDirty();
    viewer.compositors[1].MarkDirty();
}

static bool AnySequenceLoaded() {
    return (viewer.slots[0].source && viewer.slots[0].source->IsSequence()) ||
           (viewer.slots[1].source && viewer.slots[1].source->IsSequence());
}

// Union frame range over the slot sequences
static void GetSequenceRange(int &first, int &last) {
    first = INT_MAX;
    last = INT_MIN;
    for (const ViewerSlot &slot : viewer.slots) {
        if (slot.source && slot.source->IsSequence()) {
            first = std::min(first, slot.source->FirstFrame());
            last = std::max(last, slot.source->LastFrame());
        }
    }
    if (first > last) {
        first = 0;
        last = 0;
    }
}

static double PlaybackFps(const UsdStageRefPtr &stage) {
    if (stage) {
        const double fps = stage->GetFramesPerSecond();
        if (fps > 0.0) return fps;
    }
    return 24.0;
}

static int CurrentViewerFrame(UsdTimeCode currentTimeCode) {
    if (viewer.lockToTimeline) {
        return static_cast<int>(currentTimeCode.GetValue());
    }
    return viewer.transportFrame;
}

// Advance the transport when playing, looping over the union range
static void UpdatePlayback(const UsdStageRefPtr &stage) {
    if (!viewer.playing || viewer.lockToTimeline) return;
    int first, last;
    GetSequenceRange(first, last);
    if (last <= first) return;
    viewer.playbackAccumulator += ImGui::GetIO().DeltaTime * PlaybackFps(stage);
    const int advance = static_cast<int>(viewer.playbackAccumulator);
    if (advance > 0) {
        viewer.playbackAccumulator -= advance;
        viewer.transportFrame += advance;
        if (viewer.transportFrame > last) {
            viewer.transportFrame = first;
        }
    }
}

// Resolve the slot images for the current frame through the cache, keeping
// the previous buffer displayed while the new frame loads
static void ResolveSlotImages(int frame) {
    for (ViewerSlot &slot : viewer.slots) {
        if (!slot.source) continue;
        const int resolved = slot.source->ResolveFrame(frame);
        const ImageCacheKey key{slot.source->sourceId, resolved, slot.source->SettingsHash()};
        if (ImageBufferPtr buffer = viewer.cache.Get(key)) {
            if (buffer != slot.image) {
                slot.image = buffer;
                // GL texture ids can be recycled after an eviction, so the
                // compositors' id-based dirty check is not enough
                viewer.compositors[0].MarkDirty();
                viewer.compositors[1].MarkDirty();
            }
        }
        // File sources schedule the decode and read-ahead; the phase 4 render
        // sources only produce explicitly queued frames
        slot.source->RequestFrame(frame, viewer.cache);
    }
}

static void DrawSourceCombo(int slotIndex) {
    ViewerSlot &slot = viewer.slots[slotIndex];
    const char *label = slotIndex == 0 ? "##SourceA" : "##SourceB";
    const char *preview = slot.source ? slot.source->displayName.c_str() : "<none>";
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::BeginCombo(label, preview)) {
        for (const auto &source : viewer.store) {
            const bool selected = slot.source == source;
            if (ImGui::Selectable(source->displayName.c_str(), selected)) {
                AssignSourceToSlot(slotIndex, source);
            }
        }
        ImGui::EndCombo();
    }
}

static void DrawToolbar() {
    ImageCompositeParams &params = viewer.params;

    // First row: sources and compare mode
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " A")) {
        DrawModalDialog<OpenImageModalDialog>(0);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " B")) {
        DrawModalDialog<OpenImageModalDialog>(1);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.f);
    static const char *compareModes[] = {"A", "B", "Wipe", "Difference", "Side by side"};
    int mode = static_cast<int>(params.mode);
    if (ImGui::Combo("##CompareMode", &mode, compareModes, 5)) {
        params.mode = static_cast<CompareMode>(mode);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_EXCHANGE_ALT " Swap")) {
        SwapSlots();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap the A and B images");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    static const char *backgroundModes[] = {"Checker", "Black", "Grey"};
    ImGui::Combo("##Background", &params.backgroundMode, backgroundModes, 3);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_EXPAND " Fit")) {
        viewer.fitPending = true;
    }
    ImGui::SameLine();
    // 1:1 means one image pixel on one framebuffer pixel, so on hidpi displays
    // the canvas zoom is 1/framebufferScale
    const float framebufferScale = ImGui::GetIO().DisplayFramebufferScale.x;
    if (ImGui::Button("1:1")) {
        SetZoomCentered(viewer.canvas, 1.f / framebufferScale);
    }
    ImGui::SameLine();
    ImGui::Text("%.0f%%", viewer.canvas.zooming * framebufferScale * 100.0);
    if (viewer.cache.HasPendingLoads()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Loading...");
    }

    // Second row: display transform and slot sources
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##ExposureA", &params.a.exposure, -10.f, 10.f,
                       viewer.linkedDisplay ? "Exp %.2f" : "Exp A %.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##GammaA", &params.a.gamma, 0.2f, 4.f,
                       viewer.linkedDisplay ? "Gamma %.2f" : "Gamma A %.2f");
    ImGui::SameLine();
    ImGui::Checkbox("Link", &viewer.linkedDisplay);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When on, B uses A's exposure and gamma");
    if (!viewer.linkedDisplay) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.f);
        ImGui::SliderFloat("##ExposureB", &params.b.exposure, -10.f, 10.f, "Exp B %.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.f);
        ImGui::SliderFloat("##GammaB", &params.b.gamma, 0.2f, 4.f, "Gamma B %.2f");
    } else {
        params.b = params.a;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("A");
    ImGui::SameLine();
    DrawSourceCombo(0);
    ImGui::SameLine();
    ImGui::TextDisabled("B");
    ImGui::SameLine();
    DrawSourceCombo(1);
}

// Find or create the render source matching the current setup, keeping its
// rendered history when only the settings changed
static RenderImageSourcePtr FindOrCreateRenderSource(const UsdStageRefPtr &stage) {
    const std::string identity = "render|" + viewer.renderSetup.rendererPluginId.GetString() + "|" +
                                 viewer.renderSetup.productPath.GetString();
    for (const auto &existing : viewer.store) {
        if (existing->identity == identity) {
            auto renderSource = std::dynamic_pointer_cast<RenderImageSource>(existing);
            if (renderSource) {
                renderSource->SetSetup(viewer.renderSetup);
                return renderSource;
            }
        }
    }
    auto renderSource = std::make_shared<RenderImageSource>(stage, viewer.renderSetup);
    viewer.store.push_back(renderSource);
    return renderSource;
}

// Apply the product's authored resolution and camera to the setup
static void ApplyProductToSetup(const UsdStageRefPtr &stage, const SdfPath &productPath) {
    viewer.renderSetup.productPath = productPath;
    UsdRenderProduct product(stage->GetPrimAtPath(productPath));
    if (!product) return;
    GfVec2i resolution;
    if (product.GetResolutionAttr() && product.GetResolutionAttr().Get(&resolution)) {
        viewer.renderSetup.resolution = resolution;
    }
    SdfPathVector cameraTargets;
    if (product.GetCameraRel() && product.GetCameraRel().GetTargets(&cameraTargets) && !cameraTargets.empty()) {
        viewer.renderSetup.cameraPath = cameraTargets[0];
    }
}

static void InitializeRenderSetupIfNeeded(const UsdStageRefPtr &stage) {
    if (viewer.renderSetupInitialized || !stage) return;
    viewer.renderSetupInitialized = true;
    viewer.renderSetup.rendererPluginId = GetDefaultRendererId();
    // Default camera: the first one on the stage
    for (const UsdPrim &prim : stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            viewer.renderSetup.cameraPath = prim.GetPath();
            break;
        }
    }
    if (!viewer.renderRangeInitialized) {
        viewer.renderRange[0] = static_cast<int>(stage->GetStartTimeCode());
        viewer.renderRange[1] = static_cast<int>(stage->GetEndTimeCode());
        viewer.renderRangeInitialized = true;
    }
}

// Session render setup: delegate, product, camera, resolution and the render
// buttons. Renders land in the store and slot A.
static void DrawRenderSetup(const UsdStageRefPtr &stage, int currentFrame) {
    InitializeRenderSetupIfNeeded(stage);
    RenderSetup &setup = viewer.renderSetup;

    // Delegate
    ImGui::SetNextItemWidth(110.f);
    const std::string delegateName = ViewportEngine::GetRendererDisplayName(setup.rendererPluginId);
    if (ImGui::BeginCombo("##RenderDelegate", delegateName.c_str())) {
        for (const TfToken &plugin : ViewportEngine::GetRendererPlugins()) {
            if (ImGui::Selectable(ViewportEngine::GetRendererDisplayName(plugin).c_str(),
                                  plugin == setup.rendererPluginId)) {
                setup.rendererPluginId = plugin;
            }
        }
        ImGui::EndCombo();
    }
    // Render product (enumerated only while the combo is open)
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.f);
    const std::string productPreview = setup.productPath.IsEmpty() ? "<default product>" : setup.productPath.GetName();
    if (ImGui::BeginCombo("##RenderProduct", productPreview.c_str())) {
        if (ImGui::Selectable("<default product>", setup.productPath.IsEmpty())) {
            setup.productPath = SdfPath();
        }
        for (const UsdPrim &prim : stage->Traverse()) {
            if (prim.IsA<UsdRenderProduct>()) {
                if (ImGui::Selectable(prim.GetPath().GetString().c_str(), prim.GetPath() == setup.productPath)) {
                    ApplyProductToSetup(stage, prim.GetPath());
                }
            }
        }
        ImGui::EndCombo();
    }
    // Camera
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    const std::string cameraPreview = setup.cameraPath.IsEmpty() ? "<no camera>" : setup.cameraPath.GetName();
    if (ImGui::BeginCombo("##RenderCamera", cameraPreview.c_str())) {
        for (const UsdPrim &prim : stage->Traverse()) {
            if (prim.IsA<UsdGeomCamera>()) {
                if (ImGui::Selectable(prim.GetPath().GetString().c_str(), prim.GetPath() == setup.cameraPath)) {
                    setup.cameraPath = prim.GetPath();
                }
            }
        }
        ImGui::EndCombo();
    }
    // Resolution
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.f);
    int resolution[2] = {setup.resolution[0], setup.resolution[1]};
    if (ImGui::DragInt2("##RenderResolution", resolution, 4.f, 16, 16384)) {
        setup.resolution = GfVec2i(resolution[0], resolution[1]);
    }
    // The live source of the current setup, when it exists and is live
    RenderImageSourcePtr liveSource;
    for (const auto &source : viewer.store) {
        if (auto renderSource = std::dynamic_pointer_cast<RenderImageSource>(source)) {
            if (renderSource->IsInteractive()) liveSource = renderSource;
        }
    }

    // Render buttons.
    // TODO(UI): Frame/Range/Live always target slot A; they should act on the
    // slot holding the render source instead (see doc/ImageViewer.md backlog)
    ImGui::SameLine();
    const bool canRender = !setup.cameraPath.IsEmpty();
    ImGui::BeginDisabled(!canRender || bool(liveSource));
    if (ImGui::Button(ICON_FA_CAMERA " Frame")) {
        RenderImageSourcePtr renderSource = FindOrCreateRenderSource(stage);
        renderSource->QueueFrames(currentFrame, currentFrame);
        AssignSourceToSlot(0, renderSource);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render the current frame into slot A");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILM " Range")) {
        RenderImageSourcePtr renderSource = FindOrCreateRenderSource(stage);
        renderSource->QueueFrames(std::min(viewer.renderRange[0], viewer.renderRange[1]),
                                  std::max(viewer.renderRange[0], viewer.renderRange[1]));
        AssignSourceToSlot(0, renderSource);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render the frame range into slot A");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.f);
    ImGui::DragInt2("##RenderRange", viewer.renderRange, 0.2f);

    // Live mode: render the displayed frame continuously, restarting on edits
    ImGui::SameLine();
    bool live = bool(liveSource);
    ImGui::BeginDisabled(!canRender && !live);
    if (ImGui::Checkbox("Live", &live)) {
        if (live) {
            RenderImageSourcePtr renderSource = FindOrCreateRenderSource(stage);
            renderSource->SetInteractive(true);
            AssignSourceToSlot(0, renderSource);
        } else if (liveSource) {
            liveSource->SetInteractive(false);
            liveSource->SetPaused(false);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Render the displayed frame continuously, restarting on stage edits");
    if (liveSource) {
        ImGui::SameLine();
        bool paused = liveSource->IsPaused();
        if (ImGui::Button(paused ? ICON_FA_PLAY "##LivePause" : ICON_FA_PAUSE "##LivePause")) {
            liveSource->SetPaused(!paused);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(paused ? "Resume the live render" : "Pause the live render");
    }

    // Render status
    int pendingRenders = 0;
    std::string renderError;
    for (const auto &source : viewer.store) {
        if (auto renderSource = std::dynamic_pointer_cast<RenderImageSource>(source)) {
            pendingRenders += renderSource->PendingCount();
            if (renderError.empty()) renderError = renderSource->GetLastError();
        }
    }
    // No dangling SameLine when there is no status to show: the canvas is
    // drawn right after this row and must start on its own line
    if (liveSource && liveSource->IsPaused()) {
        ImGui::SameLine();
        ImGui::TextDisabled("paused");
    } else if (liveSource) {
        ImGui::SameLine();
        ImGui::TextDisabled(liveSource->IsLiveConverged() ? "converged" : "rendering...");
    } else if (pendingRenders > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("Rendering, %d frame%s left...", pendingRenders, pendingRenders > 1 ? "s" : "");
    } else if (!renderError.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", renderError.c_str());
    }
}

// Transport row and cached-frames bar, shown when a sequence is loaded
static void DrawTransport(int currentFrame) {
    int first, last;
    GetSequenceRange(first, last);

    ImGui::Checkbox("Timeline", &viewer.lockToTimeline);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Follow the editor timeline instead of the viewer transport");
    ImGui::SameLine();
    if (viewer.lockToTimeline) {
        viewer.playing = false;
        ImGui::TextDisabled("frame %d", currentFrame);
    } else {
        if (ImGui::Button(ICON_FA_STEP_BACKWARD)) {
            viewer.transportFrame = std::max(first, viewer.transportFrame - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button(viewer.playing ? ICON_FA_PAUSE : ICON_FA_PLAY)) {
            viewer.playing = !viewer.playing;
            viewer.playbackAccumulator = 0.0;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STEP_FORWARD)) {
            viewer.transportFrame = std::min(last, viewer.transportFrame + 1);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(240.f);
        viewer.transportFrame = std::max(first, std::min(viewer.transportFrame, last));
        ImGui::SliderInt("##TransportFrame", &viewer.transportFrame, first, last);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("cache %zu/%zu MB", viewer.cache.GetUsedBytes() / (1024 * 1024),
                        viewer.cache.GetBudgetBytes() / (1024 * 1024));

    // Cached-frames bar: one row per sequence slot, colored where resident
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const float barWidth = ImGui::GetContentRegionAvail().x;
    constexpr float rowHeight = 4.f;
    const int frameCount = last - first + 1;
    ImVec2 barOrigin = ImGui::GetCursorScreenPos();
    int rows = 0;
    for (int i = 0; i < 2; ++i) {
        const ImageSourcePtr &source = viewer.slots[i].source;
        if (!source || !source->IsSequence()) continue;
        const float y = barOrigin.y + rows * (rowHeight + 1.f);
        drawList->AddRectFilled(ImVec2(barOrigin.x, y), ImVec2(barOrigin.x + barWidth, y + rowHeight),
                                IM_COL32(45, 45, 50, 255));
        // Merge consecutive cached frames into single rectangles
        int runStart = -1;
        for (int f = first; f <= last + 1; ++f) {
            const bool cached = f <= last && source->HasFrame(f) &&
                                viewer.cache.Contains({source->sourceId, f, source->SettingsHash()});
            if (cached && runStart < 0) runStart = f;
            if (!cached && runStart >= 0) {
                const float x0 = barOrigin.x + barWidth * (runStart - first) / frameCount;
                const float x1 = barOrigin.x + barWidth * (f - first) / frameCount;
                drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rowHeight),
                                        i == 0 ? IM_COL32(90, 160, 90, 255) : IM_COL32(90, 120, 170, 255));
                runStart = -1;
            }
        }
        rows++;
    }
    if (rows > 0) {
        // Current frame marker over the bar(s)
        const float barHeight = rows * (rowHeight + 1.f);
        const float markerX =
            barOrigin.x + barWidth * (std::max(first, std::min(currentFrame, last)) - first + 0.5f) / frameCount;
        drawList->AddLine(ImVec2(markerX, barOrigin.y), ImVec2(markerX, barOrigin.y + barHeight),
                          IM_COL32(255, 200, 60, 255));
        ImGui::Dummy(ImVec2(barWidth, barHeight + 2.f));
    }
}

// The draggable wipe line over the image rect
static void HandleAndDrawWipe(const ImVec2 &imageMin, const ImVec2 &imageMax, ImDrawList *drawList) {
    InfiniteCanvas &canvas = viewer.canvas;
    ImageCompositeParams &params = viewer.params;
    const float wipeCanvasX = imageMin.x + params.wipe * (imageMax.x - imageMin.x);
    const ImVec2 top = canvas.CanvasToScreen(ImVec2(wipeCanvasX, imageMin.y));
    const ImVec2 bottom = canvas.CanvasToScreen(ImVec2(wipeCanvasX, imageMax.y));

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool overLine = mouse.x > top.x - 6.f && mouse.x < top.x + 6.f && mouse.y > top.y - 6.f &&
                          mouse.y < bottom.y + 6.f && canvas.widgetBoundingBox.Contains(mouse);
    if (!viewer.draggingWipe && overLine && !canvas._popupOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
        viewer.draggingWipe = true;
    }
    if (viewer.draggingWipe) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            viewer.draggingWipe = false;
        } else {
            const float mouseCanvasX = canvas.ScreenToCanvas(mouse).x;
            params.wipe = (mouseCanvasX - imageMin.x) / (imageMax.x - imageMin.x);
            params.wipe = std::max(0.f, std::min(params.wipe, 1.f));
        }
    }

    const bool highlight = overLine || viewer.draggingWipe;
    const ImU32 color = highlight ? IM_COL32(255, 200, 60, 255) : IM_COL32(220, 220, 220, 180);
    drawList->AddLine(top, bottom, color, highlight ? 3.f : 2.f);
    // A small grip at the middle of the line
    drawList->AddCircleFilled(ImVec2(top.x, (top.y + bottom.y) / 2.f), highlight ? 7.f : 5.f, color);
}

void DrawImageViewer(const UsdStageRefPtr &stage, UsdTimeCode currentTimeCode) {
    viewer.cache.Update();
    UpdatePlayback(stage);
    const int currentFrame = CurrentViewerFrame(currentTimeCode);
    // Advance the render jobs (GL thread) and other producers
    for (const auto &source : viewer.store) {
        source->Update(viewer.cache);
    }
    ResolveSlotImages(currentFrame);

    DrawToolbar();
    if (stage) {
        DrawRenderSetup(stage, currentFrame);
    }
    if (AnySequenceLoaded()) {
        DrawTransport(currentFrame);
    }

    InfiniteCanvas &canvas = viewer.canvas;
    ImageCompositeParams &params = viewer.params;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    canvas.Begin(drawList);
    canvas.HandleWheelZoom();

    // Panel background, under the images
    drawList->ChannelsSetCurrent(0);
    drawList->AddRectFilled(canvas.widgetBoundingBox.Min, canvas.widgetBoundingBox.Max, IM_COL32(25, 25, 28, 255));

    // The primary slot drives the combined-mode output resolution; with a
    // single loaded image the viewer degenerates to showing that image
    ImageBuffer *imageA = viewer.slots[0].HasValidImage() ? viewer.slots[0].image.get() : nullptr;
    ImageBuffer *imageB = viewer.slots[1].HasValidImage() ? viewer.slots[1].image.get() : nullptr;
    ImageBuffer *primary = imageA ? imageA : imageB;

    const float framebufferScale = ImGui::GetIO().DisplayFramebufferScale.x;
    const bool nearestFilter = canvas.zooming * framebufferScale >= 1.f;
    viewer.compositors[0].SetOutputFilter(nearestFilter);
    viewer.compositors[1].SetOutputFilter(nearestFilter);

    if (primary) {
        // Effective mode: fall back to the loaded slot when the other one is missing
        CompareMode mode = params.mode;
        if (!imageB) mode = CompareMode::A;
        if (!imageA) mode = CompareMode::B;

        drawList->ChannelsSetCurrent(1);
        if (mode == CompareMode::SideBySide) {
            // A on the left of B, vertically centered, with a small gap
            const float gap = 20.f;
            const ImVec2 aMin(-imageA->width - gap / 2.f, -imageA->height / 2.f);
            const ImVec2 aMax(-gap / 2.f, imageA->height / 2.f);
            const ImVec2 bMin(gap / 2.f, -imageB->height / 2.f);
            const ImVec2 bMax(gap / 2.f + imageB->width, imageB->height / 2.f);
            if (viewer.fitPending) {
                canvas.FitToBBox(ImVec2(aMin.x, std::min(aMin.y, bMin.y)),
                                 ImVec2(bMax.x, std::max(aMax.y, bMax.y)), 20.f, canvas.zoomMin, canvas.zoomMax);
                viewer.fitPending = false;
            }
            ImageCompositeParams sideParams = params;
            sideParams.mode = CompareMode::A;
            const GLuint outA = viewer.compositors[0].Composite(imageA->GetGLTexture(), 0, imageA->width,
                                                                imageA->height, sideParams);
            sideParams.mode = CompareMode::B;
            const GLuint outB = viewer.compositors[1].Composite(imageB->GetGLTexture(), imageB->GetGLTexture(),
                                                                imageB->width, imageB->height, sideParams);
            if (outA)
                drawList->AddImage((ImTextureID)((uintptr_t)outA), canvas.CanvasToScreen(aMin),
                                   canvas.CanvasToScreen(aMax));
            if (outB)
                drawList->AddImage((ImTextureID)((uintptr_t)outB), canvas.CanvasToScreen(bMin),
                                   canvas.CanvasToScreen(bMax));
            drawList->AddRect(canvas.CanvasToScreen(aMin), canvas.CanvasToScreen(aMax), IM_COL32(90, 90, 90, 255));
            drawList->AddRect(canvas.CanvasToScreen(bMin), canvas.CanvasToScreen(bMax), IM_COL32(90, 90, 90, 255));
        } else {
            // Single rect: A (or B alone), wipe and difference all render at
            // the primary resolution; the other image is sampled over the
            // same rect (stretched when the sizes differ)
            const ImVec2 imageMin(-primary->width / 2.f, -primary->height / 2.f);
            const ImVec2 imageMax(primary->width / 2.f, primary->height / 2.f);
            if (viewer.fitPending) {
                canvas.FitToBBox(imageMin, imageMax, 20.f, canvas.zoomMin, canvas.zoomMax);
                viewer.fitPending = false;
            }
            ImageCompositeParams drawParams = params;
            drawParams.mode = mode;
            const GLuint texA = imageA ? imageA->GetGLTexture() : imageB->GetGLTexture();
            const GLuint texB = imageB ? imageB->GetGLTexture() : 0;
            const GLuint composited =
                viewer.compositors[0].Composite(texA, texB, primary->width, primary->height, drawParams);
            if (composited) {
                drawList->AddImage((ImTextureID)((uintptr_t)composited), canvas.CanvasToScreen(imageMin),
                                   canvas.CanvasToScreen(imageMax));
                drawList->AddRect(canvas.CanvasToScreen(imageMin), canvas.CanvasToScreen(imageMax),
                                  IM_COL32(90, 90, 90, 255));
            }
            if (mode == CompareMode::Wipe) {
                HandleAndDrawWipe(imageMin, imageMax, drawList);
            }
        }

        // X flips between A and B when both are loaded
        if (imageA && imageB && !canvas._popupOpen && ImGui::IsKeyPressed(ImGuiKey_X) &&
            canvas.widgetBoundingBox.Contains(ImGui::GetMousePos())) {
            if (params.mode == CompareMode::A) {
                params.mode = CompareMode::B;
            } else if (params.mode == CompareMode::B) {
                params.mode = CompareMode::A;
            }
        }
    } else {
        drawList->ChannelsSetCurrent(1);
        const ImageBufferPtr &attempted = viewer.slots[0].image ? viewer.slots[0].image : viewer.slots[1].image;
        const char *message = (attempted && !attempted->error.empty()) ? attempted->error.c_str()
                                                                       : "Open or drop an image to display it here";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        const ImVec2 textPos = canvas.widgetBoundingBox.GetCenter() - textSize / 2.f;
        drawList->AddText(textPos, IM_COL32(140, 140, 140, 255), message);
    }

    if (!canvas._popupOpen && ImGui::IsKeyPressed(ImGuiKey_F) &&
        canvas.widgetBoundingBox.Contains(ImGui::GetMousePos())) {
        viewer.fitPending = true;
    }

    canvas.UpdateNavigation(!viewer.draggingWipe);
    canvas.End();
}
