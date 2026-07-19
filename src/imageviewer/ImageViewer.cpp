#include "ImageViewer.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <pxr/base/gf/camera.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>
#include <pxr/usd/usdShade/udimUtils.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include "Commands.h"

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
#include "SnapshotImageSource.h"
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
    // Per-slot render setup drafts, used until the slot holds a render source
    // (which then owns the setup). Session-only; authoring to the stage as
    // RenderSettings prims comes later.
    RenderSetup slotDraft[2];
    bool slotDraftInitialized[2] = {false, false};
    int slotRenderRange[2][2] = {{1, 24}, {1, 24}};
    // compositors[0] renders A (and the combined wipe/difference modes),
    // compositors[1] renders B for the side-by-side mode
    ImageCompositor compositors[2];
    ImageCompositeParams params;
    bool linkedDisplay = true; // B follows A's exposure/gamma

    // View menu options
    bool showPixelInspector = true;
    enum PixelFilter { FilterAuto = 0, FilterNearest, FilterLinear };
    int pixelFilter = FilterAuto;

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

// Show the source in the slot, reusing the store source with the same
// identity when one exists (the passed source is dropped then)
static void AddSourceToSlot(int slot, const ImageSourcePtr &source) {
    for (const auto &existing : viewer.store) {
        if (existing->identity == source->identity) {
            AssignSourceToSlot(slot, existing);
            return;
        }
    }
    viewer.store.push_back(source);
    AssignSourceToSlot(slot, source);
}

// Open a path in a slot: reuse the store source covering it or create one
static void OpenPathInSlot(int slot, const std::string &filePath) {
    AddSourceToSlot(slot, CreateImageSource(filePath));
}

// Set when an open request wants the panel visible, consumed by the editor
static bool showRequested = false;

void ImageViewerOpenFile(const std::string &filePath, int slot) {
    OpenPathInSlot(slot == 0 ? 0 : 1, filePath);
    showRequested = true;
}

void ImageViewerOpenAsset(const std::string &assetPath) {
    // A "<UDIM>" pattern opens as the mosaic of all its tiles
    if (UsdShadeUdimUtils::IsUdimIdentifier(assetPath)) {
        AddSourceToSlot(0, CreateUdimImageSource(assetPath));
        showRequested = true;
        return;
    }
    ImageViewerOpenFile(assetPath, 0);
}

bool ImageViewerConsumeShowRequest() {
    const bool requested = showRequested;
    showRequested = false;
    return requested;
}

void ImageViewerShutdown() {
    // Order matters: slots and store first (render sources destroy their
    // Hydra engines and draw targets), then the cache (joins the worker
    // loads, frees the buffers' GL textures), then the compositors.
    for (ViewerSlot &slot : viewer.slots) {
        slot.source = nullptr;
        slot.image = nullptr;
    }
    viewer.store.clear();
    viewer.cache.Clear();
    viewer.compositors[0].ReleaseGLResources();
    viewer.compositors[1].ReleaseGLResources();
    // The drafts hold USD handles (stage pointer, paths) that must not
    // outlive the USD teardown
    viewer.slotDraft[0] = RenderSetup();
    viewer.slotDraft[1] = RenderSetup();
}

struct SaveImageModalDialog : public ModalDialog {
    SaveImageModalDialog(ImageBufferPtr image) : image(image) { SetValidExtensions(GetImageFileExtensions()); }
    ~SaveImageModalDialog() override {}
    void Draw() override {
        DrawFileBrowser(RemainingHeight(2));
        EnsureFileBrowserDefaultExtension("exr");
        auto filePath = GetFileBrowserFilePath();
        ImGui::Text("%s", filePath.c_str());
        DrawModalButtonsOkCancel([&]() {
            if (!filePath.empty()) {
                const std::string error = SaveImageFile(image, filePath);
                if (!error.empty()) saveError = error; // shown in the slot? keep silent for now
            }
        });
    }
    const char *DialogId() const override { return "Save image"; }
    ImageBufferPtr image;
    std::string saveError;
};

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

// Step zoom: integer multiples of the device pixel going up (200% = one
// image pixel on 2x2 framebuffer pixels), reciprocals of integers going down
// (50%, 33%, 25%...). At these zooms no pixel is interpolated with the
// nearest filter.
static void StepZoom(int direction) {
    const float framebufferScale = ImGui::GetIO().DisplayFramebufferScale.x;
    const float current = viewer.canvas.zooming * framebufferScale;
    constexpr float eps = 1e-3f;
    float target;
    if (direction > 0) {
        if (current >= 1.f - eps) {
            target = std::floor(current + eps) + 1.f;
        } else {
            const int divisor = static_cast<int>(std::ceil(1.f / current - eps)) - 1;
            target = divisor <= 1 ? 1.f : 1.f / divisor;
        }
    } else {
        if (current > 1.f + eps) {
            target = std::ceil(current - eps) - 1.f;
        } else {
            const int divisor = static_cast<int>(std::floor(1.f / current + eps)) + 1;
            target = 1.f / divisor;
        }
    }
    const float zoom = target / framebufferScale;
    SetZoomCentered(viewer.canvas, std::max(viewer.canvas.zoomMin, std::min(zoom, viewer.canvas.zoomMax)));
}

// True when the composited output should use the nearest filter, given the
// zoom threshold where one output texel covers one framebuffer pixel
static bool UseNearestFilter(float zoomThreshold) {
    if (viewer.pixelFilter == ImageViewerState::FilterNearest) return true;
    if (viewer.pixelFilter == ImageViewerState::FilterLinear) return false;
    return viewer.canvas.zooming * ImGui::GetIO().DisplayFramebufferScale.x >= zoomThreshold;
}

// The viewer menu bar (the window is created with ImGuiWindowFlags_MenuBar)
static void DrawViewerMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem(ICON_FA_EXPAND " Fit", "F")) {
            viewer.fitPending = true;
        }
        const float framebufferScale = ImGui::GetIO().DisplayFramebufferScale.x;
        if (ImGui::MenuItem("Zoom 1:1")) {
            SetZoomCentered(viewer.canvas, 1.f / framebufferScale);
        }
        if (ImGui::MenuItem(ICON_FA_SEARCH_PLUS " Zoom in", "+")) {
            StepZoom(+1);
        }
        if (ImGui::MenuItem(ICON_FA_SEARCH_MINUS " Zoom out", "-")) {
            StepZoom(-1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Step zooms go through the pixel-exact zooms:\n"
                              "integer multiples of the device pixel and their reciprocals");
        }
        ImGui::Separator();
        ImGui::MenuItem("Pixel inspector", nullptr, &viewer.showPixelInspector);
        if (ImGui::BeginMenu("Pixel filter")) {
            if (ImGui::MenuItem("Auto", nullptr, viewer.pixelFilter == ImageViewerState::FilterAuto)) {
                viewer.pixelFilter = ImageViewerState::FilterAuto;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nearest at 100%% and above, linear below");
            if (ImGui::MenuItem("Nearest", nullptr, viewer.pixelFilter == ImageViewerState::FilterNearest)) {
                viewer.pixelFilter = ImageViewerState::FilterNearest;
            }
            if (ImGui::MenuItem("Linear", nullptr, viewer.pixelFilter == ImageViewerState::FilterLinear)) {
                viewer.pixelFilter = ImageViewerState::FilterLinear;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
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

static RenderImageSourcePtr GetSlotRenderSource(int slotIndex) {
    return std::dynamic_pointer_cast<RenderImageSource>(viewer.slots[slotIndex].source);
}

// Without a selected product, the width drives the height through the camera
// aperture ratio, for square pixels matching what the camera sees
static void ConformResolutionToCamera(RenderSetup &setup, int frame) {
    if (!setup.conformToCamera || !setup.productPath.IsEmpty()) return;
    UsdStageRefPtr stage(setup.stage);
    if (!stage) return;
    UsdGeomCamera camera(stage->GetPrimAtPath(setup.cameraPath));
    if (!camera) return;
    const float aspect = camera.GetCamera(UsdTimeCode(frame)).GetAspectRatio();
    if (aspect > 0.f) {
        setup.resolution[1] = std::max(16, static_cast<int>(std::lround(setup.resolution[0] / aspect)));
    }
}

static void InitializeSlotDraftIfNeeded(int slotIndex, const UsdStageRefPtr &defaultStage) {
    if (viewer.slotDraftInitialized[slotIndex] || !defaultStage) return;
    viewer.slotDraftInitialized[slotIndex] = true;
    RenderSetup &setup = viewer.slotDraft[slotIndex];
    setup.stage = defaultStage;
    setup.rendererPluginId = GetDefaultRendererId();
    // Default camera: the first one on the stage
    for (const UsdPrim &prim : defaultStage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            setup.cameraPath = prim.GetPath();
            break;
        }
    }
    viewer.slotRenderRange[slotIndex][0] = static_cast<int>(defaultStage->GetStartTimeCode());
    viewer.slotRenderRange[slotIndex][1] = static_cast<int>(defaultStage->GetEndTimeCode());
    ConformResolutionToCamera(setup, viewer.slotRenderRange[slotIndex][0]);
}

// Capture the slot's displayed image as an immutable catalog entry: appended
// to the store (never rebinding the slot) so the user can later pull it into
// either slot and compare against newer renders. Name = origin + timestamp.
static void SnapshotSlot(int slotIndex, int currentFrame) {
    const ViewerSlot &slot = viewer.slots[slotIndex];
    if (!slot.source || !slot.HasValidImage()) return;
    auto snapshot = std::make_shared<SnapshotImageSource>(slot.image);
    snapshot->sourceId = NextImageSourceId();
    // Snapshots are unique by construction, no store deduplication
    snapshot->identity = "snapshot:" + std::to_string(snapshot->sourceId);

    char timestamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::string origin = slot.source->displayName;
    std::string details;
    if (RenderImageSourcePtr renderSource = GetSlotRenderSource(slotIndex)) {
        const RenderSetup setup = renderSource->GetSetup();
        if (UsdStageRefPtr setupStage{setup.stage}) {
            origin = setupStage->GetRootLayer()->GetDisplayName();
        }
        if (!setup.cameraPath.IsEmpty()) {
            origin += " " + setup.cameraPath.GetName();
        }
        details = "\ndelegate " + ViewportEngine::GetRendererDisplayName(setup.rendererPluginId);
        if (!setup.aov.IsEmpty()) {
            details += ", aov " + setup.aov.GetString();
        }
    }
    snapshot->displayName = origin + " " + timestamp;
    const int frame = slot.source->ResolveFrame(currentFrame);
    snapshot->tooltip = "Snapshot of " + slot.source->displayName + ", frame " + std::to_string(frame) + "\n" +
                        std::to_string(slot.image->width) + "x" + std::to_string(slot.image->height) + details;
    viewer.store.push_back(snapshot);
}

// The setup the slot edits: the render source's when it holds one, the draft otherwise
static RenderSetup GetSlotSetup(int slotIndex) {
    if (RenderImageSourcePtr renderSource = GetSlotRenderSource(slotIndex)) return renderSource->GetSetup();
    return viewer.slotDraft[slotIndex];
}

static void SetSlotSetup(int slotIndex, const RenderSetup &setup) {
    // The draft follows so a future source starts from the last edited setup
    viewer.slotDraft[slotIndex] = setup;
    RenderImageSourcePtr renderSource = GetSlotRenderSource(slotIndex);
    if (!renderSource) return;
    if (renderSource == viewer.slots[1 - slotIndex].source) {
        // Both slots show the same source: edits split this slot onto its own
        // copy, e.g. comparing the beauty and the depth of one setup
        auto clone = std::make_shared<RenderImageSource>(setup);
        viewer.store.push_back(clone);
        AssignSourceToSlot(slotIndex, clone);
    } else {
        renderSource->SetSetup(setup);
    }
}

// The render source of the slot: the one it holds, a store source with the
// same identity, or a new one. Assigned to the slot.
static RenderImageSourcePtr EnsureSlotRenderSource(int slotIndex) {
    const RenderSetup setup = GetSlotSetup(slotIndex);
    if (RenderImageSourcePtr renderSource = GetSlotRenderSource(slotIndex)) {
        renderSource->SetSetup(setup);
        return renderSource;
    }
    const std::string identity = RenderImageSource::MakeIdentity(setup);
    for (const auto &existing : viewer.store) {
        if (existing->identity == identity) {
            if (auto renderSource = std::dynamic_pointer_cast<RenderImageSource>(existing)) {
                renderSource->SetSetup(setup);
                AssignSourceToSlot(slotIndex, renderSource);
                return renderSource;
            }
        }
    }
    auto renderSource = std::make_shared<RenderImageSource>(setup);
    viewer.store.push_back(renderSource);
    AssignSourceToSlot(slotIndex, renderSource);
    return renderSource;
}

// Apply the product's authored resolution and camera to the setup
static void ApplyProductToSetup(RenderSetup &setup, const UsdStageRefPtr &stage, const SdfPath &productPath) {
    setup.productPath = productPath;
    UsdRenderProduct product(stage->GetPrimAtPath(productPath));
    if (!product) return;
    GfVec2i resolution;
    if (product.GetResolutionAttr() && product.GetResolutionAttr().Get(&resolution)) {
        setup.resolution = resolution;
    }
    SdfPathVector cameraTargets;
    if (product.GetCameraRel() && product.GetCameraRel().GetTargets(&cameraTargets) && !cameraTargets.empty()) {
        setup.cameraPath = cameraTargets[0];
    }
}

// First free /parent/base, /parent/base1, ... prim path on the stage
static SdfPath FindFreePrimPath(const UsdStageRefPtr &stage, const SdfPath &parent, const std::string &base) {
    SdfPath path = parent.AppendChild(TfToken(base));
    int suffix = 1;
    while (stage->GetPrimAtPath(path)) {
        path = parent.AppendChild(TfToken(base + std::to_string(suffix++)));
    }
    return path;
}

// Author the session render setup as RenderSettings/RenderProduct/RenderVar
// prims under /Render, through the command system (undoable). Returns the
// product path the setup can point at.
static SdfPath AuthorRenderSetupToStage(const RenderSetup &setup) {
    UsdStageRefPtr stage(setup.stage);
    if (!stage) return {};
    const TfToken aov = setup.aov.IsEmpty() ? HdAovTokens->color : setup.aov;
    // The paths are decided now so the UI can point at the product immediately
    static const SdfPath renderScopePath("/Render");
    const SdfPath settingsPath = FindFreePrimPath(stage, renderScopePath, "Settings");
    const SdfPath productPath = FindFreePrimPath(stage, renderScopePath, "Product");
    const SdfPath varPath = productPath.AppendChild(TfToken(TfMakeValidIdentifier(aov.GetString())));
    const SdfPath cameraPath = setup.cameraPath;
    const GfVec2i resolution = setup.resolution;
    const std::string aovSourceName = aov.GetString();

    ExecuteAfterDraw(
        [=](UsdStageRefPtr stage) {
            UsdGeomScope::Define(stage, renderScopePath);
            UsdRenderSettings settings = UsdRenderSettings::Define(stage, settingsPath);
            UsdRenderProduct product = UsdRenderProduct::Define(stage, productPath);
            UsdRenderVar renderVar = UsdRenderVar::Define(stage, varPath);
            settings.CreateResolutionAttr(VtValue(resolution));
            settings.CreateProductsRel().SetTargets({productPath});
            product.CreateResolutionAttr(VtValue(resolution));
            product.CreateOrderedVarsRel().SetTargets({varPath});
            if (!cameraPath.IsEmpty()) {
                settings.CreateCameraRel().SetTargets({cameraPath});
                product.CreateCameraRel().SetTargets({cameraPath});
            }
            renderVar.CreateSourceNameAttr(VtValue(aovSourceName));
            // Make these the stage's render settings when none are set yet
            std::string currentSettingsPath;
            if (!stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &currentSettingsPath) ||
                currentSettingsPath.empty()) {
                stage->SetMetadata(UsdRenderTokens->renderSettingsPrimPath, settingsPath.GetString());
            }
        },
        stage);
    return productPath;
}

// The per-slot render setup popup: stage, delegate, product, camera, resolution
static void DrawSlotRenderSetupPopup(int slotIndex, int currentFrame) {
    if (!ImGui::BeginPopup("##SlotRenderSetup")) return;
    RenderSetup setup = GetSlotSetup(slotIndex);
    bool changed = false;

    // Stage: any stage open in the editor
    UsdStageRefPtr setupStage(setup.stage);
    const std::string stagePreview = setupStage ? setupStage->GetRootLayer()->GetDisplayName() : "<no stage>";
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::BeginCombo("Stage", stagePreview.c_str())) {
        for (const UsdStageRefPtr &openStage : UsdUtilsStageCache::Get().GetAllStages()) {
            if (!openStage) continue;
            const bool selected = openStage == setupStage;
            if (ImGui::Selectable(openStage->GetRootLayer()->GetIdentifier().c_str(), selected) && !selected) {
                setup.stage = openStage;
                // The product and camera belong to the previous stage
                setup.productPath = SdfPath();
                setup.cameraPath = SdfPath();
                for (const UsdPrim &prim : openStage->Traverse()) {
                    if (prim.IsA<UsdGeomCamera>()) {
                        setup.cameraPath = prim.GetPath();
                        break;
                    }
                }
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    // Delegate
    ImGui::SetNextItemWidth(220.f);
    const std::string delegateName = ViewportEngine::GetRendererDisplayName(setup.rendererPluginId);
    if (ImGui::BeginCombo("Delegate", delegateName.c_str())) {
        for (const TfToken &plugin : ViewportEngine::GetRendererPlugins()) {
            if (ImGui::Selectable(ViewportEngine::GetRendererDisplayName(plugin).c_str(),
                                  plugin == setup.rendererPluginId)) {
                setup.rendererPluginId = plugin;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    // AOV: the delegate's list once an engine exists, color before that
    ImGui::SetNextItemWidth(220.f);
    const RenderImageSourcePtr slotRenderSource = GetSlotRenderSource(slotIndex);
    const char *aovPreview = setup.aov.IsEmpty() ? "color" : setup.aov.GetText();
    if (ImGui::BeginCombo("AOV", aovPreview)) {
        const TfTokenVector aovs =
            slotRenderSource ? slotRenderSource->GetAvailableAovs() : TfTokenVector{HdAovTokens->color};
        for (const TfToken &aov : aovs) {
            const bool selected = aov == setup.aov || (setup.aov.IsEmpty() && aov == HdAovTokens->color);
            if (ImGui::Selectable(aov.GetText(), selected)) {
                setup.aov = aov;
                changed = true;
            }
        }
        if (!slotRenderSource) {
            ImGui::TextDisabled("render once to list the delegate AOVs");
        }
        ImGui::EndCombo();
    }
    setupStage = UsdStageRefPtr(setup.stage);
    if (setupStage) {
        // Render product (enumerated only while the combo is open)
        ImGui::SetNextItemWidth(220.f);
        const std::string productPreview =
            setup.productPath.IsEmpty() ? "<default product>" : setup.productPath.GetName();
        if (ImGui::BeginCombo("Product", productPreview.c_str())) {
            if (ImGui::Selectable("<default product>", setup.productPath.IsEmpty())) {
                setup.productPath = SdfPath();
                changed = true;
            }
            for (const UsdPrim &prim : setupStage->Traverse()) {
                if (prim.IsA<UsdRenderProduct>()) {
                    if (ImGui::Selectable(prim.GetPath().GetString().c_str(), prim.GetPath() == setup.productPath)) {
                        ApplyProductToSetup(setup, setupStage, prim.GetPath());
                        changed = true;
                    }
                }
            }
            ImGui::EndCombo();
        }
        // Camera
        ImGui::SetNextItemWidth(220.f);
        const std::string cameraPreview = setup.cameraPath.IsEmpty() ? "<no camera>" : setup.cameraPath.GetName();
        if (ImGui::BeginCombo("Camera", cameraPreview.c_str())) {
            for (const UsdPrim &prim : setupStage->Traverse()) {
                if (prim.IsA<UsdGeomCamera>()) {
                    if (ImGui::Selectable(prim.GetPath().GetString().c_str(), prim.GetPath() == setup.cameraPath)) {
                        setup.cameraPath = prim.GetPath();
                        changed = true;
                    }
                }
            }
            ImGui::EndCombo();
        }
    }
    // Resolution: the product provides it when one is selected; otherwise
    // the camera ratio option derives the height from the width for square
    // pixels, or both dimensions stay free
    if (setup.productPath.IsEmpty()) {
        if (ImGui::Checkbox("Camera ratio", &setup.conformToCamera)) {
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Square pixels: setting the width computes the height\n"
                              "from the camera aperture ratio");
    }
    if (setup.productPath.IsEmpty() && setup.conformToCamera) {
        ImGui::SetNextItemWidth(220.f);
        int width = setup.resolution[0];
        if (ImGui::DragInt("Width", &width, 4.f, 16, 16384)) {
            setup.resolution[0] = width;
            changed = true;
        }
        ImGui::SetNextItemWidth(220.f);
        ImGui::BeginDisabled(true);
        int height = setup.resolution[1];
        ImGui::DragInt("Height", &height);
        ImGui::EndDisabled();
    } else {
        ImGui::SetNextItemWidth(220.f);
        int resolution[2] = {setup.resolution[0], setup.resolution[1]};
        if (ImGui::DragInt2("Resolution", resolution, 4.f, 16, 16384)) {
            setup.resolution = GfVec2i(resolution[0], resolution[1]);
            changed = true;
        }
    }

    ImGui::Separator();
    // Bind the setup to the slot as a render source: Frame/Range/Live then
    // render it (they stay disabled until a slot holds a render source)
    ImGui::BeginDisabled(!setupStage || setup.cameraPath.IsEmpty());
    if (ImGui::Button(ICON_FA_PLUS " Use in slot")) {
        if (changed) {
            ConformResolutionToCamera(setup, currentFrame);
            SetSlotSetup(slotIndex, setup);
            changed = false;
        }
        EnsureSlotRenderSource(slotIndex);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Show this render setup in the slot; the Frame and Range\n"
                          "buttons render it (a stage and a camera are needed)");

    // Report the session setup onto the stage as authored render settings
    ImGui::SameLine();
    ImGui::BeginDisabled(!setupStage);
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Author to stage")) {
        const SdfPath newProductPath = AuthorRenderSetupToStage(setup);
        if (!newProductPath.IsEmpty()) {
            setup.productPath = newProductPath;
            changed = true;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create RenderSettings, RenderProduct and RenderVar prims under /Render\n"
                          "from this setup (undoable); the slot then points at the new product");

    if (changed) {
        ConformResolutionToCamera(setup, currentFrame);
        SetSlotSetup(slotIndex, setup);
    }
    ImGui::EndPopup();
}

// Asks for the frame range before a range render, prefilled from the
// setup's stage start/end timecodes at open time. The accepted range is
// remembered per slot.
struct RenderRangeModalDialog : public ModalDialog {
    RenderRangeModalDialog(int slotIndex, const UsdStageRefPtr &stage) : slotIndex(slotIndex) {
        range[0] = viewer.slotRenderRange[slotIndex][0];
        range[1] = viewer.slotRenderRange[slotIndex][1];
        if (stage) {
            range[0] = static_cast<int>(stage->GetStartTimeCode());
            range[1] = static_cast<int>(stage->GetEndTimeCode());
        }
    }
    ~RenderRangeModalDialog() override {}
    void Draw() override {
        ImGui::InputInt2("Frame range", range);
        DrawModalButtonsOkCancel([&]() {
            viewer.slotRenderRange[slotIndex][0] = range[0];
            viewer.slotRenderRange[slotIndex][1] = range[1];
            if (RenderImageSourcePtr renderSource = GetSlotRenderSource(slotIndex)) {
                renderSource->QueueFrames(std::min(range[0], range[1]), std::max(range[0], range[1]));
            }
        });
    }
    const char *DialogId() const override { return "Render range"; }
    int slotIndex;
    int range[2] = {0, 0};
};

// One slot strip: source picker, open/setup buttons, render controls, status
static void DrawSlotStrip(int slotIndex, int currentFrame, const UsdStageRefPtr &defaultStage) {
    ImGui::PushID(slotIndex);
    InitializeSlotDraftIfNeeded(slotIndex, defaultStage);
    ViewerSlot &slot = viewer.slots[slotIndex];

    // Line 1: slot letter, source combo, open file, render setup
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(slotIndex == 0 ? "A" : "B");
    ImGui::SameLine();
    const float buttonsWidth = 4.f * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
    ImGui::SetNextItemWidth(std::max(60.f, ImGui::GetContentRegionAvail().x - buttonsWidth));
    const char *preview = slot.source ? slot.source->displayName.c_str() : "<none>";
    if (ImGui::BeginCombo("##Source", preview)) {
        for (const auto &source : viewer.store) {
            const bool selected = slot.source == source;
            if (ImGui::Selectable(source->displayName.c_str(), selected)) {
                AssignSourceToSlot(slotIndex, source);
            }
            if (ImGui::IsItemHovered() && !source->tooltip.empty()) {
                ImGui::SetTooltip("%s", source->tooltip.c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN)) {
        DrawModalDialog<OpenImageModalDialog>(slotIndex);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open an image or sequence in this slot");
    ImGui::SameLine();
    ImGui::BeginDisabled(!slot.HasValidImage());
    if (ImGui::Button(ICON_FA_SAVE)) {
        DrawModalDialog<SaveImageModalDialog>(slot.image);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the displayed image to disk (.exr)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!slot.HasValidImage());
    if (ImGui::Button(ICON_FA_CAMERA_RETRO)) {
        SnapshotSlot(slotIndex, currentFrame);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snapshot the displayed image into the source list,\n"
                          "to compare against later renders");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COG)) {
        ImGui::OpenPopup("##SlotRenderSetup");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render setup of this slot: stage, delegate, camera...");
    DrawSlotRenderSetupPopup(slotIndex, currentFrame);

    // Line 2: render controls, acting on the render source the slot holds
    // (the setup popup's "Use in slot" binds one; nothing renders implicitly)
    RenderImageSourcePtr renderSource = GetSlotRenderSource(slotIndex);
    const RenderSetup setup = GetSlotSetup(slotIndex);
    const bool live = renderSource && renderSource->IsInteractive();
    const bool canRender = renderSource && !setup.cameraPath.IsEmpty() && bool(UsdStageRefPtr(setup.stage));
    ImGui::BeginDisabled(!canRender || live);
    if (ImGui::Button(ICON_FA_CAMERA " Frame")) {
        renderSource->QueueFrames(currentFrame, currentFrame);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(renderSource ? "Render the current frame into this slot"
                                       : "Render the current frame into this slot\n"
                                         "(bind a render setup first: " ICON_FA_COG " then \"Use in slot\")");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILM " Range")) {
        DrawModalDialog<RenderRangeModalDialog>(slotIndex, UsdStageRefPtr(setup.stage));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(renderSource ? "Render a frame range into this slot (asks for the range)"
                                       : "Render a frame range into this slot\n"
                                         "(bind a render setup first: " ICON_FA_COG " then \"Use in slot\")");
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool liveToggle = live;
    ImGui::BeginDisabled(!canRender && !live);
    if (ImGui::Checkbox("Live", &liveToggle)) {
        if (liveToggle && renderSource) {
            renderSource->SetInteractive(true);
        } else if (renderSource) {
            renderSource->SetInteractive(false);
            renderSource->SetPaused(false);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Render the displayed frame continuously, restarting on stage edits");
    if (renderSource && live) {
        ImGui::SameLine();
        const bool paused = renderSource->IsPaused();
        if (ImGui::Button(paused ? ICON_FA_PLAY "##Pause" : ICON_FA_PAUSE "##Pause")) {
            renderSource->SetPaused(!paused);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(paused ? "Resume the live render" : "Pause the live render");
    }

    // Line 3: status
    if (renderSource && renderSource->IsPaused()) {
        ImGui::TextDisabled("paused");
    } else if (renderSource && renderSource->IsInteractive()) {
        ImGui::TextDisabled(renderSource->IsLiveConverged() ? "converged" : "rendering...");
    } else if (renderSource && renderSource->PendingCount() > 0) {
        ImGui::TextDisabled("rendering, %d left...", renderSource->PendingCount());
    } else if (renderSource && !renderSource->GetLastError().empty()) {
        ImGui::TextDisabled("%s", renderSource->GetLastError().c_str());
    } else if (slot.HasValidImage()) {
        ImGui::TextDisabled("%dx%d", slot.image->width, slot.image->height);
    } else {
        ImGui::TextDisabled(" ");
    }
    ImGui::PopID();
}

// The middle column between the slot strips: compare mode and view controls
static void DrawMiddleControls() {
    ImageCompositeParams &params = viewer.params;
    ImGui::SetNextItemWidth(-FLT_MIN);
    static const char *compareModes[] = {"A", "B", "Wipe", "Difference", "Side by side"};
    int mode = static_cast<int>(params.mode);
    if (ImGui::Combo("##CompareMode", &mode, compareModes, 5)) {
        params.mode = static_cast<CompareMode>(mode);
    }
    if (ImGui::Button(ICON_FA_EXCHANGE_ALT " Swap", ImVec2(-FLT_MIN, 0.f))) {
        SwapSlots();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap the A and B images");
    ImGui::SetNextItemWidth(-FLT_MIN);
    static const char *backgroundModes[] = {"Checker", "Black", "Grey"};
    ImGui::Combo("##Background", &params.backgroundMode, backgroundModes, 3);
    ImGui::SetNextItemWidth(-FLT_MIN);
    static const char *channelModes[] = {"RGBA", "Red", "Green", "Blue", "Alpha", "Luminance"};
    int channel = static_cast<int>(params.channelMode);
    if (ImGui::Combo("##Channel", &channel, channelModes, 6)) {
        params.channelMode = static_cast<ChannelMode>(channel);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel isolation (keys R G B A L over the canvas)");
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
        ImGui::TextDisabled("Loading...");
    }
}

// The toolbar: A strip on the left, view controls in the middle, B strip on
// the right, then the display transform row underneath
static void DrawToolbar(const UsdStageRefPtr &defaultStage, int currentFrame) {
    ImageCompositeParams &params = viewer.params;

    if (ImGui::BeginTable("##ViewerSlots", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##SlotA", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("##Middle", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableSetupColumn("##SlotB", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawSlotStrip(0, currentFrame, defaultStage);
        ImGui::TableSetColumnIndex(1);
        DrawMiddleControls();
        ImGui::TableSetColumnIndex(2);
        DrawSlotStrip(1, currentFrame, defaultStage);
        ImGui::EndTable();
    }

    // Display transform row
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
        // Merge cached frames with consecutive numbers into single
        // rectangles. Only the frames the source actually has are visited:
        // the [first, last] integer range can be huge (photo-style numbering)
        const ImU32 runColor = i == 0 ? IM_COL32(90, 160, 90, 255) : IM_COL32(90, 120, 170, 255);
        auto drawRun = [&](int runStart, int runEnd) {
            const float x0 = barOrigin.x + barWidth * (runStart - first) / frameCount;
            // At least one pixel so sparse frames of a wide range stay visible
            const float x1 = std::max(x0 + 1.f, barOrigin.x + barWidth * (runEnd + 1 - first) / frameCount);
            drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rowHeight), runColor);
        };
        int runStart = -1, previous = 0;
        for (const int f : source->GetFrameNumbers()) {
            const bool cached = viewer.cache.Contains({source->sourceId, f, source->SettingsHash()});
            if (cached && runStart >= 0 && f != previous + 1) {
                drawRun(runStart, previous);
                runStart = f;
            } else if (cached && runStart < 0) {
                runStart = f;
            } else if (!cached && runStart >= 0) {
                drawRun(runStart, previous);
                runStart = -1;
            }
            previous = f;
        }
        if (runStart >= 0) drawRun(runStart, previous);
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

// Pixel inspector: raw linear values of both slots under the cursor, in a
// small overlay at the bottom of the canvas. imageMin/imageMax is the rect
// of the reference slot; the other slot reads through the width-normalized
// mapping the display uses.
static void DrawPixelInspector(const ImVec2 &imageMin, const ImVec2 &imageMax, ImDrawList *drawList,
                               int referenceSlot) {
    InfiniteCanvas &canvas = viewer.canvas;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (canvas._popupOpen || !canvas.widgetBoundingBox.Contains(mouse)) return;
    const ImVec2 canvasPos = canvas.ScreenToCanvas(mouse);
    if (canvasPos.x < imageMin.x || canvasPos.x >= imageMax.x || canvasPos.y < imageMin.y ||
        canvasPos.y >= imageMax.y)
        return;
    // Normalized position over the reference rect; the other slot is
    // width-normalized and vertically centered like the display shader does
    const float u = (canvasPos.x - imageMin.x) / (imageMax.x - imageMin.x);
    const float v = (canvasPos.y - imageMin.y) / (imageMax.y - imageMin.y);
    const bool bothValid = viewer.slots[0].HasValidImage() && viewer.slots[1].HasValidImage();

    char line[256];
    std::string text;
    for (int i = 0; i < 2; ++i) {
        if (!viewer.slots[i].HasValidImage()) continue;
        const ImageBuffer &image = *viewer.slots[i].image;
        float sampleV = v;
        if (i != referenceSlot && bothValid) {
            const ImageBuffer &referenceImage = *viewer.slots[referenceSlot].image;
            const float verticalScale = (static_cast<float>(image.height) * referenceImage.width) /
                                        (static_cast<float>(image.width) * referenceImage.height);
            sampleV = (v - 0.5f) / verticalScale + 0.5f;
            if (sampleV < 0.f || sampleV >= 1.f) continue; // the cursor is outside this image
        }
        const int x = std::min(image.width - 1, static_cast<int>(u * image.width));
        const int y = std::min(image.height - 1, static_cast<int>(sampleV * image.height));
        const GfHalf *pixel = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
        snprintf(line, sizeof(line), "%s%c %d,%d  %.4f %.4f %.4f %.4f", i == 0 ? "" : "   ", 'A' + i, x, y,
                 float(pixel[0]), float(pixel[1]), float(pixel[2]), float(pixel[3]));
        text += line;
    }
    if (text.empty()) return;

    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(canvas.widgetBoundingBox.Min.x + 6.f,
                     canvas.widgetBoundingBox.Max.y - textSize.y - 6.f);
    drawList->AddRectFilled(pos - ImVec2(4.f, 2.f), pos + textSize + ImVec2(4.f, 2.f), IM_COL32(15, 15, 18, 220),
                            3.f);
    drawList->AddText(pos, IM_COL32(220, 220, 220, 255), text.c_str());
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

    DrawViewerMenuBar();
    DrawToolbar(stage, currentFrame);
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

    const bool nearestFilter = UseNearestFilter(1.f);
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
            // Single rect. The displayed mode picks the reference image that
            // owns the rect: B alone shows in its own frame and resolution,
            // everything else uses A; in wipe and difference B is
            // width-normalized into A's rect, keeping its pixel aspect
            ImageBuffer *reference = (mode == CompareMode::B && imageB) ? imageB : primary;
            const ImVec2 imageMin(-reference->width / 2.f, -reference->height / 2.f);
            const ImVec2 imageMax(reference->width / 2.f, reference->height / 2.f);
            if (viewer.fitPending) {
                canvas.FitToBBox(imageMin, imageMax, 20.f, canvas.zoomMin, canvas.zoomMax);
                viewer.fitPending = false;
            }
            ImageCompositeParams drawParams = params;
            drawParams.mode = mode;
            // Output texels per canvas unit: the composite renders at the
            // density of the finer slot so a higher-resolution B keeps its
            // own resolution (its vertical extent then lands on exactly its
            // own pixel count)
            int outputWidth = reference->width;
            int outputHeight = reference->height;
            float textureDensity = 1.f;
            const bool combines = mode == CompareMode::Wipe || mode == CompareMode::Difference;
            if (imageA && imageB && combines) {
                drawParams.bVerticalScale =
                    (static_cast<float>(imageB->height) * imageA->width) /
                    (static_cast<float>(imageB->width) * imageA->height);
                if (imageB->width > imageA->width) {
                    textureDensity = static_cast<float>(imageB->width) / imageA->width;
                    outputWidth = imageB->width;
                    outputHeight = static_cast<int>(std::lround(imageA->height * textureDensity));
                }
            }
            viewer.compositors[0].SetOutputFilter(UseNearestFilter(textureDensity));
            const GLuint texA = imageA ? imageA->GetGLTexture() : imageB->GetGLTexture();
            const GLuint texB = imageB ? imageB->GetGLTexture() : 0;
            const GLuint composited =
                viewer.compositors[0].Composite(texA, texB, outputWidth, outputHeight, drawParams);
            if (composited) {
                drawList->AddImage((ImTextureID)((uintptr_t)composited), canvas.CanvasToScreen(imageMin),
                                   canvas.CanvasToScreen(imageMax));
                drawList->AddRect(canvas.CanvasToScreen(imageMin), canvas.CanvasToScreen(imageMax),
                                  IM_COL32(90, 90, 90, 255));
            }
            if (mode == CompareMode::Wipe) {
                HandleAndDrawWipe(imageMin, imageMax, drawList);
            }
            if (viewer.showPixelInspector) {
                DrawPixelInspector(imageMin, imageMax, drawList, reference == imageB && imageB ? 1 : 0);
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
    // Step zoom on the +/- keys over the canvas
    if (!canvas._popupOpen && canvas.widgetBoundingBox.Contains(ImGui::GetMousePos())) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) StepZoom(+1);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) StepZoom(-1);
    }
    // Channel isolation keys: R G B A L toggle the channel, back to RGBA on
    // the second press
    if (!canvas._popupOpen && canvas.widgetBoundingBox.Contains(ImGui::GetMousePos())) {
        auto toggleChannel = [&](ChannelMode channel) {
            params.channelMode = params.channelMode == channel ? ChannelMode::RGBA : channel;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_R)) toggleChannel(ChannelMode::Red);
        if (ImGui::IsKeyPressed(ImGuiKey_G)) toggleChannel(ChannelMode::Green);
        if (ImGui::IsKeyPressed(ImGuiKey_B)) toggleChannel(ChannelMode::Blue);
        if (ImGui::IsKeyPressed(ImGuiKey_A)) toggleChannel(ChannelMode::Alpha);
        if (ImGui::IsKeyPressed(ImGuiKey_L)) toggleChannel(ChannelMode::Luminance);
    }

    canvas.UpdateNavigation(!viewer.draggingWipe);
    canvas.End();
}
