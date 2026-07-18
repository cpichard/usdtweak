#include "ImageViewer.h"

#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include "FileBrowser.h"
#include "FileImageSource.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ImageCompositor.h"
#include "InfiniteCanvas.h"
#include "ModalDialogs.h"

// One image slot: what is loaded and what is being loaded. The latest request
// made while a load is in flight is queued.
struct ViewerSlot {
    ImageBufferPtr image;
    std::future<ImageBufferPtr> pendingLoad;
    std::string queuedPath;

    bool HasValidImage() const { return image && image->IsValid(); }
};

// The viewer is unique by design: one canvas, two compared slots (A/B),
// living for the whole application session.
struct ImageViewerState {
    InfiniteCanvas canvas;
    ViewerSlot slots[2];
    // compositors[0] renders A (and the combined wipe/difference modes),
    // compositors[1] renders B for the side-by-side mode
    ImageCompositor compositors[2];
    ImageCompositeParams params;
    bool linkedDisplay = true; // B follows A's exposure/gamma

    bool fitPending = false;
    bool draggingWipe = false;

    ImageViewerState() {
        // Inspecting pixels calls for a much wider zoom range than the node editor
        canvas.zoomMin = 1.f / 64.f;
        canvas.zoomMax = 64.f;
    }
};

static ImageViewerState viewer;

static void RequestImageLoad(int slot, const std::string &filePath) {
    ViewerSlot &s = viewer.slots[slot];
    if (s.pendingLoad.valid()) {
        s.queuedPath = filePath;
    } else {
        s.pendingLoad = LoadImageFileAsync(filePath);
    }
}

void ImageViewerOpenFile(const std::string &filePath, int slot) {
    RequestImageLoad(slot == 0 ? 0 : 1, filePath);
}

static void UpdatePendingLoads() {
    for (int i = 0; i < 2; ++i) {
        ViewerSlot &slot = viewer.slots[i];
        if (slot.pendingLoad.valid() &&
            slot.pendingLoad.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            slot.image = slot.pendingLoad.get();
            viewer.compositors[0].MarkDirty();
            viewer.compositors[1].MarkDirty();
            viewer.fitPending |= slot.image->IsValid();
            if (!slot.queuedPath.empty()) {
                slot.pendingLoad = LoadImageFileAsync(slot.queuedPath);
                slot.queuedPath.clear();
            }
        }
    }
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
                RequestImageLoad(slot, filePath);
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
    int mode = static_cast<int>(params.mode == CompareMode::SideBySide ? CompareMode::SideBySide
                                                                       : params.mode);
    // Combo order: A, B, Wipe, Difference, Side by side (matches CompareMode)
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
    if (viewer.slots[0].pendingLoad.valid() || viewer.slots[1].pendingLoad.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Loading...");
    }

    // Second row: display transform
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
    // Image names at the end of the row
    for (int i = 0; i < 2; ++i) {
        if (viewer.slots[i].HasValidImage()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%c: %dx%d %s", 'A' + i, viewer.slots[i].image->width,
                                viewer.slots[i].image->height, viewer.slots[i].image->sourceName.c_str());
        }
    }
}

// The draggable wipe line over the image rect. Returns true when the mouse is
// interacting with it so the caller can keep other click handling away.
static void HandleAndDrawWipe(const ImVec2 &imageMin, const ImVec2 &imageMax, ImDrawList *drawList) {
    InfiniteCanvas &canvas = viewer.canvas;
    ImageCompositeParams &params = viewer.params;
    const float wipeCanvasX = imageMin.x + params.wipe * (imageMax.x - imageMin.x);
    const ImVec2 top = canvas.CanvasToScreen(ImVec2(wipeCanvasX, imageMin.y));
    const ImVec2 bottom = canvas.CanvasToScreen(ImVec2(wipeCanvasX, imageMax.y));

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool overLine = mouse.x > top.x - 6.f && mouse.x < top.x + 6.f && mouse.y > top.y - 6.f &&
                          mouse.y < bottom.y + 6.f && canvas.widgetBoundingBox.Contains(mouse);
    if (!viewer.draggingWipe && overLine && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
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

void DrawImageViewer() {
    UpdatePendingLoads();
    DrawToolbar();

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
        if (imageA && imageB && ImGui::IsKeyPressed(ImGuiKey_X) &&
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

    if (ImGui::IsKeyPressed(ImGuiKey_F) && canvas.widgetBoundingBox.Contains(ImGui::GetMousePos())) {
        viewer.fitPending = true;
    }

    canvas.UpdateNavigation(!viewer.draggingWipe);
    canvas.End();
}
