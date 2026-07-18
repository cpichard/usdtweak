#include "ImageViewer.h"

#include <chrono>
#include <string>

#include "FileBrowser.h"
#include "FileImageSource.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ImageCompositor.h"
#include "InfiniteCanvas.h"
#include "ModalDialogs.h"

// The viewer is unique by design: one canvas, one image (A/B slots arrive in
// phase 2), living for the whole application session.
struct ImageViewerState {
    InfiniteCanvas canvas;
    ImageBufferPtr image;
    ImageCompositor compositor;
    ImageDisplayParams display;

    // One load in flight; the latest request made meanwhile is queued
    std::future<ImageBufferPtr> pendingLoad;
    std::string queuedPath;

    bool fitPending = false;

    ImageViewerState() {
        // Inspecting pixels calls for a much wider zoom range than the node editor
        canvas.zoomMin = 1.f / 64.f;
        canvas.zoomMax = 64.f;
    }
};

static ImageViewerState viewer;

static void RequestImageLoad(const std::string &filePath) {
    if (viewer.pendingLoad.valid()) {
        viewer.queuedPath = filePath;
    } else {
        viewer.pendingLoad = LoadImageFileAsync(filePath);
    }
}

void ImageViewerOpenFile(const std::string &filePath) { RequestImageLoad(filePath); }

static void UpdatePendingLoad() {
    if (viewer.pendingLoad.valid() &&
        viewer.pendingLoad.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        viewer.image = viewer.pendingLoad.get();
        viewer.compositor.MarkDirty();
        viewer.fitPending = viewer.image->IsValid();
        if (!viewer.queuedPath.empty()) {
            viewer.pendingLoad = LoadImageFileAsync(viewer.queuedPath);
            viewer.queuedPath.clear();
        }
    }
}

struct OpenImageModalDialog : public ModalDialog {
    OpenImageModalDialog() { SetValidExtensions(GetImageFileExtensions()); }
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
                RequestImageLoad(filePath);
            }
        });
    }
    const char *DialogId() const override { return "Open image"; }
};

// Change the zoom while keeping the canvas point at the widget center in place
static void SetZoomCentered(InfiniteCanvas &canvas, float zoom) {
    const ImVec2 center(-canvas.scrolling.x / canvas.zooming, -canvas.scrolling.y / canvas.zooming);
    canvas.zooming = zoom;
    canvas.scrolling = ImVec2(-center.x * zoom, -center.y * zoom);
}

static void DrawToolbar() {
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open")) {
        DrawModalDialog<OpenImageModalDialog>();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##Exposure", &viewer.display.exposure, -10.f, 10.f, "Exp %.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##Gamma", &viewer.display.gamma, 0.2f, 4.f, "Gamma %.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    static const char *backgroundModes[] = {"Checker", "Black", "Grey"};
    ImGui::Combo("##Background", &viewer.display.backgroundMode, backgroundModes, 3);
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
    if (viewer.image && viewer.image->IsValid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d %s", viewer.image->width, viewer.image->height, viewer.image->sourceName.c_str());
    }
    if (viewer.pendingLoad.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Loading...");
    }
}

void DrawImageViewer() {
    UpdatePendingLoad();
    DrawToolbar();

    InfiniteCanvas &canvas = viewer.canvas;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    canvas.Begin(drawList);
    canvas.HandleWheelZoom();

    // Panel background, under the image
    drawList->ChannelsSetCurrent(0);
    drawList->AddRectFilled(canvas.widgetBoundingBox.Min, canvas.widgetBoundingBox.Max, IM_COL32(25, 25, 28, 255));

    if (viewer.image && viewer.image->IsValid()) {
        ImageBuffer &image = *viewer.image;
        // The image is centered on the canvas origin, 1 canvas unit = 1 image pixel
        const ImVec2 imageMin(-image.width / 2.f, -image.height / 2.f);
        const ImVec2 imageMax(image.width / 2.f, image.height / 2.f);
        if (viewer.fitPending) {
            canvas.FitToBBox(imageMin, imageMax, 20.f, canvas.zoomMin, canvas.zoomMax);
            viewer.fitPending = false;
        }
        const float framebufferScale = ImGui::GetIO().DisplayFramebufferScale.x;
        viewer.compositor.SetOutputFilter(canvas.zooming * framebufferScale >= 1.f);
        const GLuint composited =
            viewer.compositor.Composite(image.GetGLTexture(), image.width, image.height, viewer.display);
        if (composited) {
            drawList->ChannelsSetCurrent(1);
            drawList->AddImage((ImTextureID)((uintptr_t)composited), canvas.CanvasToScreen(imageMin),
                               canvas.CanvasToScreen(imageMax));
            drawList->AddRect(canvas.CanvasToScreen(imageMin), canvas.CanvasToScreen(imageMax),
                              IM_COL32(90, 90, 90, 255));
        }
    } else {
        drawList->ChannelsSetCurrent(1);
        const char *message = (viewer.image && !viewer.image->error.empty())
                                  ? viewer.image->error.c_str()
                                  : "Open an image to display it here";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        const ImVec2 textPos = canvas.widgetBoundingBox.GetCenter() - textSize / 2.f;
        drawList->AddText(textPos, IM_COL32(140, 140, 140, 255), message);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F) && canvas.widgetBoundingBox.Contains(ImGui::GetMousePos())) {
        viewer.fitPending = true;
    }

    canvas.UpdateNavigation(true);
    canvas.End();
}
