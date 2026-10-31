#include "ManipulatorToolbox.h"
#include "Constants.h"
#include "Viewport.h"
#include "Commands.h"

/** The following code for creating a toolbar is coming from github, slightly adapted
    for our manipulator buttons.
    https://github.com/ocornut/imgui/issues/2648
*/
/*
// FIXME-DOCK: This is not yet working (imgui_internal's StartMouseMovingWindowOrNode() is not finished!)
// Custom widget to forcefully enable docking of a window when dragging (when there is not title bar)
// In the meanwhile, dragging from the corner triangle or top of a toolbar will do the same.
void UndockWidget(ImVec2 icon_size, ImGuiAxis axis)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    ImVec2 size = icon_size;
    size[axis] = ImFloor(size[axis] * 0.30f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##Undock", size);

    const bool is_hovered = ImGui::IsItemHovered();
    const bool is_active = ImGui::IsItemActive();
    if (is_active)
        ImGui::StartMouseMovingWindowOrNode(window, window->DockNode, true);

    const ImU32 col = ImGui::GetColorU32(is_active ? ImGuiCol_ButtonActive : is_hovered ? ImGuiCol_ButtonHovered : ImGuiCol_TextDisabled, 0.4f);
    window->DrawList->AddRectFilled(p, p + size, col);
}
*/

// Toolbar test [Experimental]
// Usage:
// {
//   static ImGuiAxis toolbar1_axis = ImGuiAxis_X; // Your storage for the current direction.
//   DockingToolbar("Toolbar1", &toolbar1_axis);
// }
static void DockingToolbar(const char* name, Viewport &viewport, ImGuiAxis* p_toolbar_axis)
{
    const ImVec4 defaultColor(0.1, 0.1, 0.1, 0.9);
    const ImVec4 selectedColor(ColorButtonHighlight);
    // [Option] Automatically update axis based on parent split (inside of doing it via right-click on the toolbar)
    // Pros:
    // - Less user intervention.
    // - Avoid for need for saving the toolbar direction, since it's automatic.
    // Cons: 
    // - This is currently leading to some glitches.
    // - Some docking setup won't return the axis the user would expect.
    const bool TOOLBAR_AUTO_DIRECTION_WHEN_DOCKED = true;

    // ImGuiAxis_X = horizontal toolbar
    // ImGuiAxis_Y = vertical toolbar
    ImGuiAxis toolbar_axis = *p_toolbar_axis;

    // 1. We request auto-sizing on one axis
    // Note however this will only affect the toolbar when NOT docked.
    ImVec2 requested_size = (toolbar_axis == ImGuiAxis_X) ? ImVec2(-1.0f, 0.0f) : ImVec2(0.0f, -1.0f);
    ImGui::SetNextWindowSize(requested_size);

    // 2. Specific docking options for toolbars.
    // Currently they add some constraint we ideally wouldn't want, but this is simplifying our first implementation
    ImGuiWindowClass window_class;
    window_class.DockingAllowUnclassed = true;
    window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoCloseButton;
    window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_HiddenTabBar; // ImGuiDockNodeFlags_NoTabBar // FIXME: Will need a working Undock widget for _NoTabBar to work
    window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoDockingSplit;
    window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoDockingOverMe;
    window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoDockingOverOther;
    if (toolbar_axis == ImGuiAxis_X)
        window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoResizeY;
    else
        window_class.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoResizeX;
    ImGui::SetNextWindowClass(&window_class);

    // 3. Begin into the window
    const float font_size = ImGui::GetFontSize();
    const ImVec2 icon_size(ImFloor(font_size * 1.7f), ImFloor(font_size * 1.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, toolbar_axis == ImGuiAxis_X ? ImVec2(10.f, 2.0f) : ImVec2(2.0f, 10.0f));
    ImGui::Begin(name, NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    // 4. Overwrite node size
    ImGuiDockNode* node = ImGui::GetWindowDockNode();
    if (node != NULL)
    {
        // Overwrite size of the node
        ImGuiStyle& style = ImGui::GetStyle();
        const ImGuiAxis toolbar_axis_perp = (ImGuiAxis)(toolbar_axis ^ 1);
        const float TOOLBAR_SIZE_WHEN_DOCKED = style.WindowPadding[toolbar_axis_perp] * 2.0f + icon_size[toolbar_axis_perp];
        node->WantLockSizeOnce = true;
        node->Size[toolbar_axis_perp] = node->SizeRef[toolbar_axis_perp] = TOOLBAR_SIZE_WHEN_DOCKED;

        if (TOOLBAR_AUTO_DIRECTION_WHEN_DOCKED)
            if (node->ParentNode && node->ParentNode->SplitAxis != ImGuiAxis_None)
                toolbar_axis = (ImGuiAxis)(node->ParentNode->SplitAxis ^ 1);
    }
    
    // 5. Populate tab bar
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 5.0f));

    ImGui::PushStyleColor(ImGuiCol_Button, viewport.IsChosenManipulator<MouseHoverManipulator>() ? selectedColor : defaultColor);
    if (ImGui::Button(ICON_FA_MOUSE_POINTER, icon_size)) {
        ExecuteAfterDraw<ViewportsSelectMouseHoverManipulator>();
    }
    ImGui::PopStyleColor();
    if (toolbar_axis == ImGuiAxis_X)
        ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, viewport.IsChosenManipulator<PositionManipulator>() ? selectedColor : defaultColor);
    if (ImGui::Button(ICON_FA_ARROWS_ALT, icon_size)) {
        ExecuteAfterDraw<ViewportsSelectPositionManipulator>();
    }
    ImGui::PopStyleColor();
    if (toolbar_axis == ImGuiAxis_X)
        ImGui::SameLine();
    
    ImGui::PushStyleColor(ImGuiCol_Button, viewport.IsChosenManipulator<RotationManipulator>() ? selectedColor : defaultColor);
    if (ImGui::Button(ICON_FA_SYNC_ALT, icon_size)) {
        ExecuteAfterDraw<ViewportsSelectRotationManipulator>();
    }
    ImGui::PopStyleColor();
    if (toolbar_axis == ImGuiAxis_X)
        ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, viewport.IsChosenManipulator<ScaleManipulator>() ? selectedColor : defaultColor);
    if (ImGui::Button(ICON_FA_EXPAND_ARROWS_ALT, icon_size)) {
        ExecuteAfterDraw<ViewportsSelectScaleManipulator>();
    }
    ImGui::PopStyleColor();
    
    
    ImGui::PopStyleVar(2);

    // 6. Context-menu to change axis
    if (node == NULL || !TOOLBAR_AUTO_DIRECTION_WHEN_DOCKED)
    {
        if (ImGui::BeginPopupContextWindow())
        {
            ImGui::TextUnformatted(name);
            ImGui::Separator();
            if (ImGui::MenuItem("Horizontal", "", (toolbar_axis == ImGuiAxis_X)))
                toolbar_axis = ImGuiAxis_X;
            if (ImGui::MenuItem("Vertical", "", (toolbar_axis == ImGuiAxis_Y)))
                toolbar_axis = ImGuiAxis_Y;
            ImGui::EndPopup();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();

    // Output user stored data
    *p_toolbar_axis = toolbar_axis;
}


void DrawManipulatorToolbox(Editor *editor) {
    static ImGuiAxis toolbar1_axis = ImGuiAxis_X; // Your storage for the current direction.
    DockingToolbar("ManipulatorToolbox", editor->GetViewport(), &toolbar1_axis);
}
