
#include "ConnectionEditor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "Commands.h"
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>
#include <pxr/usd/usd/primRange.h>
#include <iostream>
#include <stack>
#include <queue>
#include <algorithm>
#include <unordered_set>
#include <map>
#include <numeric>

/*
 Notes on the connection editor.
    One approach was to read the whole scene at each frame to deduce/rewrite the graph of connected attributes, however after a few tests
    with the Usd and Sdf api it appears neither of them are fast enough for doing so on a moderate sized scene and low performance machine.
    So we need a different approach, potentially reading only what is needed, ideally without using caching and TfNotice
    We could add "buttons" to look for connections and keep the graph in a local memory
 */

PXR_NAMESPACE_USING_DIRECTIVE

///
///
///             NEW VERSION WORK IN PROGRESS
///
///

// TODO: Do we want undo/redo on the node editor ? moving nodes, deleting, etc ?
// TODO: node selection (multi)

// Encapsulate a prim and its graphical informations like position, size, shape, etc
// Do we want to show inputs/outputs/ all parameters ?? just one connector ???
struct UsdPrimNode {
    UsdPrimNode(UsdPrim prim_) : prim(prim_) {
        primPath = prim.GetPath();
//        std::cout << "USDPRIM " << sizeof(UsdPrim) << std::endl;
//        std::cout << "NodalPrim " << sizeof(UsdPrimNode) << std::endl;
//        std::cout << "SdfPath " << sizeof(SdfPath) << std::endl;
    }
    
    ImVec2 position; // 8 bytes
    UsdPrim prim; // 32 bytes -> might not be a good idea to store that, but let's keep it for now until we know what we need to display
    SdfPath primPath; // 8 bytes
    // Properties could be tokens or path ?? path could be useful for connecting the properties
    // Copy of the property names to avoid iterating on them (could reduce the name size)
    std::vector<SdfPath> properties;
    bool selected = false;
};

// TODO: what do we need to draw a bezier curve between nodes
// TODO: how is the interaction with bezier curves done ??
struct NodeConnection {
    NodeConnection(const SdfPath &begin_, const SdfPath &end_) : begin(begin_), end(end_) {}
    SdfPath begin;
    SdfPath end;     // 8 bytes
};

// TODO black board sheet should also store the canvas position, that would be easier when switching

struct ConnectionsSheet {
    
    // Should we keep the root prim ?? the root path under which we would create new prims
    // TODO we can have a default sheet without a stage
    ConnectionsSheet() {}
    ConnectionsSheet(const UsdPrim &prim) : rootPrim(prim) {}
    
    std::vector<UsdPrimNode> nodes;
    
    // Connections are updated
    std::vector<NodeConnection> connections;
    
    void AddNodes(const std::vector<UsdPrim> &prims) {
        // TODO Make sure there are no duplicates,
        // we don't want to have the same prim multiple time in nodes
        for (const UsdPrim &prim:prims) {
            nodes.emplace_back(prim);
        }
    }
    
    UsdStageWeakPtr GetCurrentStage() { return rootPrim ? rootPrim.GetStage() : nullptr; }
    
    // Update the node inputs/outputs positions and the connections such that this is visually
    // coherent
    void Update() {
        // For all the nodes, update positions and connections and attributes ??
        // We need to know the stage
        //static std::vector<std::pair<SdfPath, SdfPath>> connections;
        connections.clear();
        // Should we just process the whole stage (or layer)
        // it's too slow unfortunately

//        UsdPrimRange range = rootPrim.GetStage()->Traverse();
//        for (const auto &prim : range) {
//            for (const UsdAttribute &attr: prim.GetAttributes()) {
//                if (attr.HasAuthoredConnections()) {
//                    //properties.push_back(attr);
//                    SdfPathVector sources;
//                    attr.GetConnections(&sources);
//                    for (const auto &path: sources) {
//                        // fill the graph
//                        connections.emplace_back(attr.GetPath(), path);
//                        // (SdfPath, SdfPath)
//                        // (SdfPath, position)
//
//                    }
//                }
//            }
//        }
        

        for (auto &node:nodes) {
            //static std::vector<UsdAttribute> properties;
            //properties.clear();
            node.properties.clear();
            // TODO:
            //    Looking at the usd code, GetAuthoredProperties is doing too many things and allocating/release too much memory
            //    This is not going to scale to many nodes for interactive rendering, find a way to get the connections fast
            //const auto properties = node.prim.GetAuthoredProperties();
            for (const UsdAttribute &attr: node.prim.GetAttributes()) {
                // TODO check if it's an input/output or generic connectable parameter
                //node.properties.push_back(attr.GetName().GetString());
                node.properties.push_back(attr.GetPath());
                if (attr.HasAuthoredConnections()) {
                    //properties.push_back(attr);
                    SdfPathVector sources;
                    attr.GetConnections(&sources);
                    for (const auto &path:sources) {
                        // TODO: should we just get the begin and end position of the curve ?
                        // how can we interact with the curve ? to delete connection for example ?
                        connections.emplace_back(attr.GetPath(), path);
                    }
                }
            }
        }
        //@std::cout << "Connections " << connections.size() << std::endl;
    }

    // root prim to know the stage and the prim to add other prim under
    UsdPrim rootPrim;
};

// One set of sheets per stage.
// Each sheets contains nodes which are representations of prims
struct StageSheets {
    
    using SheetID = std::string; // TODO might be more efficient with int
    StageSheets() = delete;
    StageSheets(UsdStageWeakPtr stage) : defaultSheet(stage ? stage->GetDefaultPrim() : UsdPrim()) {
        // We have a default empty sheet which is linked to the root
        // make sense to call it default ??
        if (stage) {
            selectedSheet = "default";
            sheets[selectedSheet] = ConnectionsSheet(stage->GetDefaultPrim());
            sheetNames.push_back(selectedSheet);
        }
    }

    // We expect to have max around 20 sheets, that would be a lot already
    std::unordered_map<SheetID, ConnectionsSheet> sheets;
    SheetID selectedSheet;
    std::vector<std::string> sheetNames;
    
    //
    SheetID CreateSheet(const UsdPrim &rootPrim) {
        // TODO: CreateSheet should create a new sheet using the rootPrimPath as the anchor
        // This will be useful when we will create nodes directly
        //selectedSheet = rootPrimPath.GetString();
        
        SheetID sheetID = rootPrim.GetPath().GetString();
        const std::string sheetName = rootPrim.GetPath().GetString();
        
        auto sheetIt = sheets.find(sheetID);
        if (sheetIt == sheets.end()) {
            sheetNames.push_back(sheetName);
            sheets[sheetID] = ConnectionsSheet(rootPrim);
        }
        return sheetID;
    }
    
    void DeleteSheet(const SheetID &sheetName) {
        // Remove in sheetNames
        // Remove in sheets
        // Update selected
        // TODO
    }

    //
    ConnectionsSheet & GetSheet(const SheetID &sheetID) { return sheets[sheetID]; }
    
   
    //
    void SetSelectedSheet(const SheetID &sheetID) { selectedSheet = sheetID; }
    
    //
    ConnectionsSheet & GetSelectedSheet() {
        return sheets[selectedSheet];
    }
    
    const SheetID & GetSelectedSheetID() { return selectedSheet;}
    
    const std::string & GetSelectedSheetName() { return selectedSheet; }
    
    // Needed by the combo box
    const std::vector<std::string> & GetSheetNames() const {
        return sheetNames;
    }
    
    ConnectionsSheet defaultSheet; // default root sheet, can't be removed
};


// This might end up as an exposed class, but for now we keep it hidden in the translation unit
struct NodeConnectionEditorData {
    
    NodeConnectionEditorData() {}
    // Storing the unique identifier as a key won't keep the data if we unload and reload the stage
    // with undo/redo
    std::unordered_map<void const *, StageSheets> stageSheets;
    
    StageSheets * GetSheets(UsdStageWeakPtr stage) {
        if (stage) {
            if (stageSheets.find(stage->GetUniqueIdentifier()) == stageSheets.end()) {
                stageSheets.insert({stage->GetUniqueIdentifier(), StageSheets(stage)});
            }
            return & stageSheets.at(stage->GetUniqueIdentifier());
        }
        return nullptr;
    }
};

// global variable ...
static NodeConnectionEditorData editorData;

// TODO create manipulators for editing node positions, selections, etc



struct ConnectionsEditorCanvas { // rename to InfiniteCanvas ??
    
    // Testing with event and state machine
    uint8_t state = 0; // current edition state of the canvas, (selecting, moving node, zoom/pan?)
    uint8_t event = 0; // events on the current rendered frame
    
    // ??? events we need
    enum Events : uint8_t {
        IDLE = 0,
        CANVAS_CLICKED,
        CANVAS_CLICKED_PANNING,
        CANVAS_CLICKED_ZOOMING,
        NODE_CLICKED,
        CONNECTOR_CLICKED,
        CLICK_RELEASED,
        SELECT_PRIM_CLICKED,
    };
    
    // ???
    enum States : uint8_t {
        HOVERING_CANVAS, //
        SELECTING_REGION,
        CANVAS_ZOOMING,
        CANVAS_PANING,
        SELECTING_NODE,
        MOVING_NODE,
        CONNECTING_NODES,
    };
    
    void Begin(ImDrawList* drawList_) {
        
        // Reset state
        event  = Events::IDLE; // reset event
        hasSelectedNodes = false; // reset selected nodes

        drawList = drawList_;
        ImGuiContext& g = *GImGui;
        // Current widget position, in absolute coordinates. (relative to the main window)
        widgetOrigin = ImGui::GetCursorScreenPos(); // canvasOrigin, canvasSize
        // WindowSize returns the size of the whole window, including tabs that we have to remove to get the widget size.
        // GetCursorPos gives the current position in window coordinates
        widgetSize = ImGui::GetWindowSize() - ImGui::GetCursorPos() - g.Style.WindowPadding; // Should also add the borders
        originOffset = (widgetSize / 2.f); // in window Coordinates
        drawList->ChannelsSplit(2); // Foreground and background
        // TODO: we might want to use only widgetBB and get rid of widgetOrigin and widgetSize
        widgetBoundingBox.Min = widgetOrigin;
        widgetBoundingBox.Max = widgetOrigin + widgetSize;
        drawList->PushClipRect(widgetBoundingBox.Min, widgetBoundingBox.Max);
        // There is no overlapping of widgets unfortunateluy
        //if (ImGui::InvisibleButton("canvas", widgetBoundingBox.GetSize())) {
        //    std::cout << "Canvas clicked" << std::endl;
        //}
        if (widgetBoundingBox.Contains(ImGui::GetMousePos())) {
            // Click on the canvas TODO test bounding box
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                    event = Events::CANVAS_CLICKED_PANNING;
                } else {
                    event = Events::CANVAS_CLICKED;
                }
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                    event = Events::CANVAS_CLICKED_ZOOMING;
                    ImGuiIO& io = ImGui::GetIO();
                    zoomClick = io.MouseClickedPos[ImGuiMouseButton_Right];
                }
            } else if (ImGui::IsMouseReleased(0) || ImGui::IsMouseReleased(1)) { // TODO Should be any button ??
                event = Events::CLICK_RELEASED;
            }
        } else {
            event = Events::CLICK_RELEASED;
        }
    }
    
    void End() {
        drawList->ChannelsMerge();
        drawList->PopClipRect();
    };

    // Adjust zoom and scroll so all nodes are visible in the current widget.
    // Must be called after Begin() so widgetSize is up to date.
    void FitView(const ConnectionsSheet &sheet) {
        if (sheet.nodes.empty() || widgetSize.x <= 0.f || widgetSize.y <= 0.f) return;

        constexpr float fitPadding  = 40.f; // screen-space margin around the graph
        constexpr float headerHeight = 50.f;
        constexpr float lineHeight   = 15.f;

        ImVec2 bboxMin( FLT_MAX,  FLT_MAX);
        ImVec2 bboxMax(-FLT_MAX, -FLT_MAX);
        for (const auto &node : sheet.nodes) {
            float h = static_cast<float>(node.properties.size() + 2) * lineHeight + headerHeight;
            ImVec2 nodeMin = node.position + ImVec2(-80.f, -h / 2.f);
            ImVec2 nodeMax = node.position + ImVec2( 80.f,  h / 2.f);
            bboxMin.x = std::min(bboxMin.x, nodeMin.x);
            bboxMin.y = std::min(bboxMin.y, nodeMin.y);
            bboxMax.x = std::max(bboxMax.x, nodeMax.x);
            bboxMax.y = std::max(bboxMax.y, nodeMax.y);
        }

        const float bboxW = bboxMax.x - bboxMin.x;
        const float bboxH = bboxMax.y - bboxMin.y;
        if (bboxW <= 0.f || bboxH <= 0.f) return;

        // Compute zoom to fit, then clamp to a reasonable range.
        const float fitZoomX = (widgetSize.x - 2.f * fitPadding) / bboxW;
        const float fitZoomY = (widgetSize.y - 2.f * fitPadding) / bboxH;
        zooming = std::max(0.05f, std::min(std::min(fitZoomX, fitZoomY), 5.f));

        // Set scroll so the bbox center maps to the widget center.
        // CanvasToWindow(pos) = pos*zoom + scroll + originOffset
        // We want CanvasToWindow(center) = originOffset  →  scroll = -center*zoom
        const ImVec2 bboxCenter = (bboxMin + bboxMax) * 0.5f;
        scrolling = bboxCenter * (-zooming);
    }

    // What we call canvas is the infinite normalized region
    inline ImVec2 CanvasToWindow(const ImVec2 &posInCanvas) {
        return posInCanvas * zooming + scrolling + originOffset;
    }

    inline ImVec2 WindowToCanvas(const ImVec2 &posInWindow) {
        return  (posInWindow - scrolling - originOffset) / zooming;
    }
    
    inline ImVec2 WindowToScreen(const ImVec2 &posInWindow) {
        return posInWindow + widgetOrigin;
    }
    
    inline ImVec2 ScreenToWindow(const ImVec2 &posInScreen) {
        return posInScreen - widgetOrigin;
    }

    // The imgui draw functions are expressed in absolute screen coordinates.
    inline ImVec2 CanvasToScreen(const ImVec2 &posInCanvas) {
        return WindowToScreen(CanvasToWindow(posInCanvas));
    }
    
    inline ImVec2 ScreenToCanvas(const ImVec2 &posInScreen) {
        return WindowToCanvas(ScreenToWindow(posInScreen));
    }

    // Zoom using posInScreen as the origin of the zoom
    inline void ZoomFromPosition(const ImVec2 &posInScreen, const ImVec2 &deltaInScreen) {
        // Offset, zoom, -offset
        auto posInCanvas = ScreenToCanvas(posInScreen);
        auto zoomDelta = -0.002 * (deltaInScreen.x + deltaInScreen.y);
        zooming *= 1.f + zoomDelta;
        auto posInCanvasAfterZoom = ScreenToCanvas(posInScreen);
        auto diff = CanvasToScreen(posInCanvas) - CanvasToScreen(posInCanvasAfterZoom);
        scrolling -= diff;
        // Debug: check the zoom origin position remains the same
        drawList->AddCircle(CanvasToScreen(posInCanvas), 10, IM_COL32(255, 0, 255, 255));
    }
    
    // TODO Clipping of nodes (do not draw them in the first place)
    
    
    // For debugging the widget size
    void DrawBoundaries() {
        // Test the center of the virtual canvas
        ImVec2 centerScreen = CanvasToScreen(ImVec2(0.F, 0.f));
        drawList->AddCircle(centerScreen, 10, 0xFFFFFFFF);
        drawList->AddLine(widgetOrigin, ImVec2(0, widgetSize.y) + widgetOrigin, 0xFFFFFFFF);
        drawList->AddLine(widgetOrigin, ImVec2(widgetSize.x, 0) + widgetOrigin, 0xFFFFFFFF);
        drawList->AddLine(ImVec2(0, widgetSize.y) + widgetOrigin, ImVec2(widgetSize.x, 0) + widgetOrigin, 0xFFFFFFFF);
        drawList->AddLine(ImVec2(0, widgetSize.y) + widgetOrigin, ImVec2(widgetSize.x, widgetSize.y) + widgetOrigin, 0xFFFFFFFF);
        drawList->AddLine(ImVec2(widgetSize.x, widgetSize.y) + widgetOrigin, ImVec2(widgetSize.x, 0) + widgetOrigin, 0xFFFFFFFF);
    }
    
    void DrawGrid() {
        drawList->ChannelsSetCurrent(0); // Background
        const float gridSpacing = 50.f;
        const ImU32 gridColor = IM_COL32(200, 200, 200, 40);
        // Find the first visible line of the grid
        auto gridOrigin = ScreenToCanvas(widgetOrigin);
        gridOrigin = ImVec2(ceilf(gridOrigin.x/gridSpacing)*gridSpacing, ceilf(gridOrigin.y/gridSpacing)*gridSpacing);
        gridOrigin = CanvasToScreen(gridOrigin);
        auto gridSize0 = CanvasToScreen(ImVec2(0.f, 0.f));
        auto gridSize1 = CanvasToScreen(ImVec2(gridSpacing, gridSpacing));
        auto gridSize = gridSize1 - gridSize0;
        
        // Draw in screen space
        for (float x = gridOrigin.x; x<widgetOrigin.x+widgetSize.x; x+=gridSize.x) {
            drawList->AddLine(ImVec2(x, widgetOrigin.y),
                              ImVec2(x, widgetOrigin.y+widgetSize.y), gridColor);
        }
        for (float y = gridOrigin.y; y<widgetOrigin.y+widgetSize.y; y+=gridSize.y) {
            drawList->AddLine(ImVec2(0.f, y),
                              ImVec2(widgetOrigin.x+widgetSize.x, y), gridColor);
        }
    }
    
    static ImU32 GetNodeHeaderColor(const UsdPrim &prim) {
        static const TfToken materialToken("Material");
        static const TfToken shaderToken("Shader");
        static const TfToken nodeGraphToken("NodeGraph");
        const TfToken t = prim.GetTypeName();
        if (t == materialToken)  return IM_COL32(110, 45, 110, 255);
        if (t == shaderToken)    return IM_COL32(45,  85, 135, 255);
        if (t == nodeGraphToken) return IM_COL32(45, 110,  75, 255);
        return                          IM_COL32(65,  65,  85, 255);
    }

    void DrawNode(UsdPrimNode &node) {
        ImGuiContext& g = *GImGui;
        ImGuiIO& io = ImGui::GetIO();
        drawList->ChannelsSetCurrent(1); // Foreground
        constexpr float headerHeight = 50.f;
        constexpr float connectorSize = 14.f; // square connector size
        
        //static std::vector<UsdAttribute> properties;
        //properties.clear();
        const std::vector<SdfPath> &properties = node.properties;
        // TODO:
        //    Looking at the usd code, GetAuthoredProperties is doing too many things and allocating/release to much memory
        //    This is not going to scale to many nodes, find a way to get the connections fast
        //const auto properties = node.prim.GetAuthoredProperties();
        //        for (const UsdAttribute &attr: node.prim.GetAttributes()) {
        //            if (attr.HasAuthoredConnections()) {
        //                properties.push_back(attr);
        ////                SdfPathVector sources;
        ////                attr.GetConnections(&sources);
        ////                for (const auto &path:sources) {
        ////                    properties.emplace_back(path);
        ////                }
        //            }
        //        }
        
        int nbNodes = static_cast<int>(properties.size());
        float nodeHeight = (nbNodes+2)*(g.FontSize + 2.f) + headerHeight; // 50 == header size
        ImVec2 nodeMin = ImVec2(-80.f, -nodeHeight/2.f) + node.position;
        ImVec2 nodeMax = ImVec2(80.f, nodeHeight/2.f) + node.position;
        // The invisible button will trigger the sliders if it's not clipped
        ImRect nodeBoundingBox(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax));
        nodeBoundingBox.ClipWith(widgetBoundingBox);
        
        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), 0xFF090920, 4.0f);
        const ImVec2 headerMax = ImVec2(nodeMax.x, nodeMin.y + headerHeight);
        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(headerMax),
                                GetNodeHeaderColor(node.prim), 4.0f, ImDrawFlags_RoundCornersTop);
        drawList->AddRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), node.selected ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 255, 255), 4.0f);
        
        // TODO: add padding, truncate name if too long, add tooltip
        drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(nodeMin), IM_COL32(255, 255, 255, 255), node.prim.GetName().GetText());
        
        // Check if the user clicked on the node and update the event.
        // The event might be again updated later on, on the connectors as well, that's how choosing the event is implemented
        if (ImGui::IsMouseClicked(0)) {
            if (nodeBoundingBox.Contains(ImGui::GetMousePos())) {
                event = Events::NODE_CLICKED; // Node clicked
                nodeClicked = &node;
            }
        }

        // "Select prim" button — top-right corner of the header, same size as connectors.
        // Overrides NODE_CLICKED when the button is the actual click target.
        {
            const ImVec2 btnMin(nodeMax.x - connectorSize - 2.f, nodeMin.y + 2.f);
            const ImVec2 btnMax(nodeMax.x - 2.f, nodeMin.y + connectorSize + 2.f);
            ImRect btnBB(CanvasToScreen(btnMin), CanvasToScreen(btnMax));
            btnBB.ClipWith(widgetBoundingBox);
            const bool btnHovered = btnBB.Contains(ImGui::GetMousePos());
            const ImU32 btnColor = btnHovered ? IM_COL32(255, 200, 60, 255) : IM_COL32(180, 140, 40, 160);
            drawList->AddRectFilled(CanvasToScreen(btnMin), CanvasToScreen(btnMax), btnColor, 2.f);
            drawList->AddText(g.Font, g.FontSize * zooming * 0.85f, CanvasToScreen(btnMin), IM_COL32(255, 255, 255, 255), "S");
            if (ImGui::IsMouseClicked(0) && btnBB.Contains(ImGui::GetMousePos())) {
                event = Events::SELECT_PRIM_CLICKED;
                nodeClicked = &node;
            }
        }

        // Draw properties
        const ImVec2 inputStartPos = nodeMin + ImVec2(10.f, headerHeight);
        for (int i=0; i<properties.size(); i++) {
            const float linePos = i * (g.FontSize + 2.f); // 2.f == padding
            
            // Input connector position min max
            const auto conMin = ImVec2(nodeMin.x - connectorSize/2, inputStartPos.y + 1.f + linePos);
            const auto conMax = ImVec2(nodeMin.x + connectorSize/2, inputStartPos.y + 15.f + linePos);
            ImU32 connectorColor = IM_COL32(255, 255, 255, 255);
            // We test if the mouse is hovering the input connector when we are connecting nodes
            if (state == CONNECTING_NODES) {
                ImRect connectorBoundingBox(CanvasToScreen(conMin), CanvasToScreen(conMax));
                if (connectorBoundingBox.Contains(ImGui::GetMousePos())) {
                    connectorHeadClicked = properties[i];
                    connectorColor = IM_COL32(255, 127, 17, 255);
                }
            }

            drawList->AddRectFilled(CanvasToScreen(conMin), CanvasToScreen(conMax), connectorColor);
            
            // Output connector min/max
            connectorColor = connectorTailClicked == properties[i] ? IM_COL32(255, 127, 127, 255): IM_COL32(255, 255, 255, 255);
            const auto outConMin = ImVec2(nodeMax.x - connectorSize/2, inputStartPos.y + 1.f + linePos);
            const auto outConMax = ImVec2(nodeMax.x + connectorSize/2, inputStartPos.y + 15.f + linePos);
            drawList->AddRectFilled(CanvasToScreen(outConMin), CanvasToScreen(outConMax), connectorColor);
            
            // Checking if the output connector is clicked
            if (ImGui::IsMouseClicked(0)) {
                ImRect connectorBoundingBox(CanvasToScreen(outConMin), CanvasToScreen(outConMax));
                if (connectorBoundingBox.Contains(ImGui::GetMousePos())) {
                    event = Events::CONNECTOR_CLICKED; // Node clicked
                    connectorTailClicked = properties[i];
                }
            }
            
            const auto textPos = inputStartPos + ImVec2(0, linePos); // TODO: padding and text size
            //propertie[i].
            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255), properties[i].GetNameToken().GetText());
            
            // Update positions of the connectors
            connectorPositions[properties[i]] = ImVec4(nodeMin.x, inputStartPos.y + 9.f + linePos, nodeMax.x, inputStartPos.y + 9.f + linePos);
        }
        
        // TODO: will we need invisible buttons ? code left here
        //ImGui::SetCursorScreenPos(nodeBoundingBox.Min);
        //        if (buttonBoundingBox.GetArea() > 0.f && ImGui::InvisibleButton("node", nodeBoundingBox.GetSize())) {
        //            //node.selected = !node.selected; // TODO add info to event
        //            event = Events::NODE_CLICKED; // Node clicked
        //        }

        // Update node data based on current state
        if (state == SELECTING_REGION) { // Region selection
            if (GetSelectionRegion().Overlaps(ImRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax)))) {
                node.selected = true;
            } else {
                node.selected = false;
            }
        } else if (state == MOVING_NODE && nodeClicked) {
            if (nodeClicked->selected && node.selected) {
                ImGuiIO& io = ImGui::GetIO();
                node.position = node.position + io.MouseDelta/zooming;
            } else if (!nodeClicked->selected && nodeClicked == &node) {
                // TODO this could be a function UpdateNodePosition(node)
                nodeClicked->position = nodeClicked->position + io.MouseDelta/zooming;
            }
        }

        // Update
        hasSelectedNodes |= node.selected;
    }
    
    // Draw so test nodes to see how the coordinate system works as all are expressed in screen
//    void DrawNodeTest(ImVec2 &nodePos) {
//        ImGuiContext& g = *GImGui;
//        ImGuiIO& io = ImGui::GetIO();
//        //ImGuiWindow* window = g.CurrentWindow;
//        // Give the node a position in canvas coordinates
//        drawList->ChannelsSetCurrent(1); // Foreground
//
//        // Node is centered here, is it a good idea in the end ??
//        int nbNodes = 8;
//
//        const float headerHeight = 50.f;
//        const float conSize = 14.f; // square connector size
//        // Compute node size (node height)
//        float nodeHeight = (nbNodes+2)*(g.FontSize + 2.f) + headerHeight; // 50 == header size
//
//        // Node bounding box
//        ImVec2 nodeMin = ImVec2(-80.f, -nodeHeight/2.f) + nodePos;
//        ImVec2 nodeMax = ImVec2(80.f, nodeHeight/2.f) + nodePos;
//
//        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), 0xFF090920, 4.0f);
//        drawList->AddRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), IM_COL32(255, 255, 255, 255), 4.0f);
//
//        // TODO: add padding, truncate name if too long, add tooltip
//        drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(nodeMin), IM_COL32(255, 255, 255, 255), "Node test");
//
//        // Draw inputs
//        const char *inputs[5] = {"size", "color", "height", "width", "radius"};
//
//        // Start of the input list
//        const ImVec2 inputStartPos = nodeMin + ImVec2(10.f, headerHeight);
//        for (int i=0; i<5; i++) {
//            const float linePos = i * (g.FontSize + 2.f); // 2.f == padding
//            // Connector position min max
//            const auto conMin = ImVec2(nodeMin.x - conSize/2, inputStartPos.y + 1.f + linePos);
//            const auto conMax = ImVec2(nodeMin.x + conSize/2, inputStartPos.y + 15.f + linePos);
//            drawList->AddRectFilled(CanvasToScreen(conMin), CanvasToScreen(conMax), IM_COL32(255, 255, 255, 255));
//
//            const auto textPos = inputStartPos + ImVec2(0, linePos); // TODO: padding and text size
//            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255), inputs[i]);
//        }
//
//        // Draw outputs, outputs start after the inputs
//        const char *outputs[3] = {"output", "material", "shader"};
//        const ImVec2 outputStartPos = nodeMin + ImVec2(10.f, headerHeight) + ImVec2(80.f, 0.f);
//        for (int i=0; i<3; i++) {
//            const float linePos = (i+6) * (g.FontSize + 2.f); // 2.f == padding, 6 == number of input nodes +1
//            // Connector position min max
//            const auto conMin = ImVec2(nodeMax.x - conSize/2, inputStartPos.y + 1.f + linePos);
//            const auto conMax = ImVec2(nodeMax.x + conSize/2, inputStartPos.y + 15.f + linePos);
//            drawList->AddRectFilled(CanvasToScreen(conMin), CanvasToScreen(conMax), IM_COL32(255, 255, 255, 255));
//
//            const auto textPos = outputStartPos + ImVec2(0, linePos); // TODO: padding and text size
//            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255), outputs[i]);
//        }
//
//        // The invisible button will trigger the sliders
//        ImRect buttonBoundingBox(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax));
//        buttonBoundingBox.ClipWith(widgetBoundingBox);
//
//        //drawList->AddRect(buttonBoundingBox.Min, buttonBoundingBox.Max, IM_COL32(255, 0, 0, 255), 4.0f);
//
//        ImGui::SetCursorScreenPos(buttonBoundingBox.Min);
//        if (buttonBoundingBox.GetArea() > 0.f && ImGui::InvisibleButton("node", buttonBoundingBox.GetSize())) {
//        }
//        // TODO we should have a IsItemDragging instead, first click on the item to make it "drag"
//
//        if (ImGui::IsItemHovered())
//        {
//            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
//                nodePos = nodePos + io.MouseDelta/zooming;
//        }
//    }
    
//    void DrawNodeTest() {
//        static ImVec2 nodePos(0.f, 0.f); // Position in the canvas
//        DrawNodeTest(nodePos);
//    }
    
    void DrawSheet(ConnectionsSheet &sheet) {
        ImGuiContext& g = *GImGui;
        ImGuiIO& io = ImGui::GetIO();
        currentStage = sheet.GetCurrentStage();
        connectorPositions.clear();
        //ImGuiWindow* window = g.CurrentWindow;
        // Give the node a position in canvas coordinates
        drawList->ChannelsSetCurrent(1); // Foreground
        //static ImVec2 nodePos(0.f, 0.f); // Position in the canvas
        for (auto &node : sheet.nodes) {
            DrawNode(node);
        }
        drawList->ChannelsSetCurrent(0); // Background
        for (const auto &con:sheet.connections) {
            const auto arrowHead = connectorPositions.find(con.begin);
            if (arrowHead != connectorPositions.end()) {
                const auto arrowTail = connectorPositions.find(con.end);
                if (arrowTail != connectorPositions.end()) {
                    ImVec2 p2a(arrowHead->second.x, arrowHead->second.y);
                    ImVec2 p1a(arrowTail->second.z, arrowTail->second.w);
                    ImVec2 p2b(arrowHead->second.x-150, arrowHead->second.y);
                    ImVec2 p1b(arrowTail->second.z+150, arrowTail->second.w);
                    ImVec2 mouse = ImGui::GetMousePos();
                    auto color = IM_COL32(255, 255, 255, 255);
                    //ImVec2 closest = ImBezierCubicClosestPointCasteljau(CanvasToScreen(p1a), CanvasToScreen(p1b), CanvasToScreen(p2b), CanvasToScreen(p2a), mouse, O.2);
                    drawList->AddBezierCubic(CanvasToScreen(p1a), CanvasToScreen(p1b), CanvasToScreen(p2b), CanvasToScreen(p2a), color, 2);
                }
            }
        }
        
        // Show connecting node
        if (state == CONNECTING_NODES) {
            const auto arrowTail = connectorPositions.find(connectorTailClicked);
            ImVec2 p1(arrowTail->second.z, arrowTail->second.w);
            drawList->AddLine(CanvasToScreen(p1), ImGui::GetMousePos(),IM_COL32(255, 127, 127, 255));
        }
    }
    
    // TODO
    void DrawRegionSelection() {
        if (state == SELECTING_REGION) {
            // Draw rect
            // As we iterate on all the nodes anyway, we might as well test if they are in the region selection and
            // add them in a special list _aboutToBeSelected ....
            // And here we just have to draw the region
            const ImRect selectionRegion = GetSelectionRegion();
            drawList->AddRect(selectionRegion.Min, selectionRegion.Max, IM_COL32(0, 255, 0, 127));
        }
    }
    
    // Update the editing state given the last event and the current state
    // Not very readable, but it works
    void UpdateState() {
        ImGuiIO& io = ImGui::GetIO();
        if (state ==States::HOVERING_CANVAS) {
            if (event == SELECT_PRIM_CLICKED) {
                if (nodeClicked) {
                    ExecuteAfterDraw<EditorSetSelection>(nodeClicked->prim.GetStage(), nodeClicked->primPath);
                }
            } else if (event == NODE_CLICKED) {
                state = SELECTING_NODE; // Single node selection,
            } else if (event == CANVAS_CLICKED) {
                state = SELECTING_REGION; // Region selection
                selectionOrigin = ImGui::GetMousePos();
            } else if (event == CANVAS_CLICKED_PANNING) {
                state = CANVAS_PANING;
            } else if (event == CANVAS_CLICKED_ZOOMING) {
                state = CANVAS_ZOOMING;
            } else if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
            } else if (event == CONNECTOR_CLICKED) {
                state = CONNECTING_NODES;
            }
        } else if (state == SELECTING_REGION) { // Region selection
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
                // TODO PROCESS SELECTED NODE
                // Or we could just select them if the state is region selection ??
            }
        } else if (state == SELECTING_NODE) {
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
                // TODO PROCESS SELECTED NODE
                // SELECT / DESELECT NODE
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f)) {
                state = MOVING_NODE;
                // TODO Store position ??
            }
        } else if (state == CANVAS_PANING) {
            if (event == CLICK_RELEASED || !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                state = HOVERING_CANVAS;
            }
            // Update scrolling
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
                scrolling = scrolling + io.MouseDelta;
            }
        } else if (state == CANVAS_ZOOMING) {
            if (event == CLICK_RELEASED || !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                state = HOVERING_CANVAS;
            }
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
                ZoomFromPosition(zoomClick, io.MouseDelta);
            }
        } else if (state == MOVING_NODE) {
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
                nodeClicked = nullptr;
            }
        } else if (state == CONNECTING_NODES) {
            if (event == CLICK_RELEASED) {
                if (connectorHeadClicked != SdfPath::EmptyPath()) {
                    // Launch a command !
                    ExecuteAfterDraw<AttributeConnect>(currentStage, connectorTailClicked, connectorHeadClicked);
                }
                state = HOVERING_CANVAS;
                connectorTailClicked = SdfPath::EmptyPath();
                connectorHeadClicked = SdfPath::EmptyPath();
            }
        }
        // Debug
        ImGuiContext& g = *GImGui;
        std::string stateDebug = "State " + std::to_string(state);
        drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(ImVec2(0.2, 0.2)), IM_COL32(255, 255, 255, 255), stateDebug.c_str());
    }
    
    void ProcessAction() {
        // NODE_SELECTION and RELEASED : action select node, should we add actions for selections ??
        // We certainly want actions for linking nodes
    }
    
    ImRect GetSelectionRegion() const {
        const ImVec2 mousePos = ImGui::GetMousePos();
        const ImVec2 Min(std::min(selectionOrigin.x, mousePos.x), std::min(selectionOrigin.y, mousePos.y));
        const ImVec2 Max(std::max(selectionOrigin.x, mousePos.x), std::max(selectionOrigin.y, mousePos.y));
        return ImRect(Min, Max);
    }
    
    // We want the origin to be at the center of the canvas
    // so we apply an offset which depends on the canvas size
    ImVec2 originOffset = ImVec2(0.0f, 0.0f);
    ImVec2 scrolling = ImVec2(0.0f, 0.0f); // expressed in screen space
    // Zooming starting at 1 means we have an equivalence between screen and canvas unit, this
    // is probably not what we want.
    // Zooming 10 means 10 times bigger than the pixel size
    float zooming = 1.f; // TODO: make sure zooming is never 0
    ImVec2 zoomClick = ImVec2(0.0f, 0.0f); // Zoom origin
    ImVec2 selectionOrigin; // TODO this could be union with zoom click (origin)
    ImVec2 widgetOrigin = ImVec2(0.0f, 0.0f);  // canvasOrigin, canvasSize in screen coordinates
    ImVec2 widgetSize = ImVec2(0.0f, 0.0f);
    ImRect widgetBoundingBox;
    UsdPrimNode *nodeClicked = nullptr;
    
    SdfPath connectorTailClicked;
    SdfPath connectorHeadClicked;

    // Connectors positions
    std::unordered_map<SdfPath, ImVec4, SdfPath::Hash> connectorPositions; // 2 in 2 out
    // std::unordered_map<SdfPath, ImVec2, SdfPath::Hash> outputsPositions;

    bool hasSelectedNodes = false; // computed at each frame
    
    ImDrawList* drawList = nullptr;
    
    UsdStageWeakPtr currentStage;
};


// Sugiyama layered graph auto-layout with connected-component splitting and
// per-type subgraph layout within each component.
//
//  1. Find connected components (Union-Find, undirected).
//  2. Within each component: group nodes by USD prim type name.
//     Run Sugiyama on each type group using only intra-group edges
//     (avoids cross-type cycles, e.g. Material ↔ Shader).
//     Stack type groups vertically, centred on the component origin.
//  3. Arrange connected components horizontally, centred on canvas (0,0).
static void AutoLayout(ConnectionsSheet &sheet) {
    const int n = static_cast<int>(sheet.nodes.size());
    if (n == 0) return;

    constexpr float nodeWidth    = 160.f; // node spans position.x ± 80
    constexpr float hGap         = 80.f;  // horizontal gap between layer columns
    constexpr float vGap         = 30.f;  // vertical gap between nodes in a column
    constexpr float typeGroupGap = 80.f;  // vertical gap between type groups in a component
    constexpr float compGap      = 120.f; // horizontal gap between connected components
    constexpr float headerHeight = 50.f;
    constexpr float lineHeight   = 15.f;  // ~ImGui fontSize + 2px padding

    auto getNodeHeight = [&](int idx) -> float {
        return static_cast<float>(sheet.nodes[idx].properties.size() + 2) * lineHeight + headerHeight;
    };

    // --- Build attribute-path and prim-path lookup maps ---
    std::unordered_map<SdfPath, int, SdfPath::Hash> attrToIdx, primToIdx;
    primToIdx.reserve(n);
    attrToIdx.reserve(n * 8);
    for (int i = 0; i < n; i++) {
        primToIdx[sheet.nodes[i].primPath] = i;
        for (const auto &prop : sheet.nodes[i].properties)
            attrToIdx[prop] = i;
    }

    auto findNode = [&](const SdfPath &p) -> int {
        auto it = attrToIdx.find(p);
        if (it != attrToIdx.end()) return it->second;
        auto it2 = primToIdx.find(p.GetPrimPath());
        return it2 != primToIdx.end() ? it2->second : -1;
    };

    // --- 1. Find connected components via Union-Find (path-halving) ---
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);

    auto findComp = [&](int x) -> int {   // iterative path halving
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int x, int y) {
        x = findComp(x); y = findComp(y);
        if (x != y) parent[x] = y;
    };

    for (const auto &con : sheet.connections) {
        int src = findNode(con.end), dst = findNode(con.begin);
        if (src >= 0 && dst >= 0 && src != dst)
            unite(src, dst);
    }

    // Group global node indices by component root (std::map → deterministic order).
    std::map<int, std::vector<int>> components;
    for (int i = 0; i < n; i++)
        components[findComp(i)].push_back(i);

    // --- Sugiyama layout for one type group ---
    // Writes node positions centred at (0, 0); returns {totalWidth, maxColHeight}.
    auto layoutGroup = [&](const std::vector<int> &grp) -> std::pair<float, float> {
        const int g = static_cast<int>(grp.size());
        if (g == 0) return {0.f, 0.f};

        std::unordered_map<int, int> globalToLocal;
        globalToLocal.reserve(g);
        for (int li = 0; li < g; li++) globalToLocal[grp[li]] = li;

        std::vector<std::unordered_set<int>> dn(g), up(g);
        for (const auto &con : sheet.connections) {
            int src = findNode(con.end), dst = findNode(con.begin);
            if (src < 0 || dst < 0 || src == dst) continue;
            auto si = globalToLocal.find(src), di = globalToLocal.find(dst);
            if (si == globalToLocal.end() || di == globalToLocal.end()) continue;
            dn[si->second].insert(di->second);
            up[di->second].insert(si->second);
        }

        // Layer assignment (Kahn / longest-path variant).
        std::vector<int> layer(g, 0);
        std::vector<int> inDeg(g);
        for (int i = 0; i < g; i++) inDeg[i] = static_cast<int>(up[i].size());
        std::queue<int> q;
        for (int i = 0; i < g; i++) if (inDeg[i] == 0) q.push(i);
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v : dn[u]) {
                layer[v] = std::max(layer[v], layer[u] + 1);
                if (--inDeg[v] == 0) q.push(v);
            }
        }

        const int maxLayer = *std::max_element(layer.begin(), layer.end());
        std::vector<std::vector<int>> layers(maxLayer + 1);
        for (int i = 0; i < g; i++) layers[layer[i]].push_back(i);

        // Barycenter crossing minimisation — 3 alternating passes.
        auto buildPos = [&]() {
            std::vector<int> pos(g, 0);
            for (int l = 0; l <= maxLayer; l++)
                for (int p = 0; p < static_cast<int>(layers[l].size()); p++)
                    pos[layers[l][p]] = p;
            return pos;
        };
        for (int pass = 0; pass < 3; pass++) {
            {
                auto pos = buildPos();
                for (int l = 1; l <= maxLayer; l++) {
                    std::vector<std::pair<float, int>> order;
                    order.reserve(layers[l].size());
                    for (int li : layers[l]) {
                        float bc = 0.f; int cnt = 0;
                        for (int u : up[li]) { bc += static_cast<float>(pos[u]); cnt++; }
                        order.push_back({cnt > 0 ? bc / static_cast<float>(cnt) : 0.f, li});
                    }
                    std::stable_sort(order.begin(), order.end());
                    layers[l].clear();
                    for (auto &[b, li] : order) layers[l].push_back(li);
                }
            }
            {
                auto pos = buildPos();
                for (int l = maxLayer - 1; l >= 0; l--) {
                    std::vector<std::pair<float, int>> order;
                    order.reserve(layers[l].size());
                    for (int li : layers[l]) {
                        float bc = 0.f; int cnt = 0;
                        for (int d : dn[li]) { bc += static_cast<float>(pos[d]); cnt++; }
                        order.push_back({cnt > 0 ? bc / static_cast<float>(cnt) : 0.f, li});
                    }
                    std::stable_sort(order.begin(), order.end());
                    layers[l].clear();
                    for (auto &[b, li] : order) layers[l].push_back(li);
                }
            }
        }

        // Coordinate assignment — centred at (0, 0).
        const float totalWidth = static_cast<float>(maxLayer) * (nodeWidth + hGap) + nodeWidth;
        const float startX     = -totalWidth / 2.f;

        float maxColHeight = 0.f;
        for (int l = 0; l <= maxLayer; l++) {
            float colH = vGap * static_cast<float>(std::max(0, static_cast<int>(layers[l].size()) - 1));
            for (int li : layers[l]) colH += getNodeHeight(grp[li]);
            maxColHeight = std::max(maxColHeight, colH);
        }
        for (int l = 0; l <= maxLayer; l++) {
            const float cx = startX + static_cast<float>(l) * (nodeWidth + hGap) + nodeWidth / 2.f;
            float colH = vGap * static_cast<float>(std::max(0, static_cast<int>(layers[l].size()) - 1));
            for (int li : layers[l]) colH += getNodeHeight(grp[li]);
            float y = -colH / 2.f;
            for (int li : layers[l]) {
                const float h = getNodeHeight(grp[li]);
                sheet.nodes[grp[li]].position = ImVec2(cx, y + h / 2.f);
                y += h + vGap;
            }
        }
        return {totalWidth, maxColHeight};
    };

    // --- 2. Layout each connected component ---
    // For each component: group by type, run Sugiyama per type group,
    // stack type groups vertically. Returns component {width, height}.
    struct CompInfo { float width = 0.f; float height = 0.f; };
    std::vector<CompInfo>          compInfos;
    std::vector<std::vector<int>>  compNodes; // global node indices per component

    for (auto &[root, nodeIndices] : components) {
        // Group by prim type within this component.
        std::map<std::string, std::vector<int>> typeGroups;
        for (int idx : nodeIndices)
            typeGroups[sheet.nodes[idx].prim.GetTypeName().GetString()].push_back(idx);

        // Layout each type group; nodes are initially centred at (0, 0).
        std::vector<std::pair<float, float>> groupSizes;
        std::vector<const std::vector<int>*> groupPtrs;
        float compWidth = 0.f;
        for (auto &[typeName, grp] : typeGroups) {
            groupSizes.push_back(layoutGroup(grp));
            groupPtrs.push_back(&grp);
            compWidth = std::max(compWidth, groupSizes.back().first);
        }

        // Stack type groups vertically, centred on y = 0.
        float totalH = typeGroupGap * static_cast<float>(std::max(0, static_cast<int>(groupSizes.size()) - 1));
        for (auto &[w, h] : groupSizes) totalH += h;

        float yTop = -totalH / 2.f;
        for (int gi = 0; gi < static_cast<int>(groupPtrs.size()); gi++) {
            const float yShift = yTop + groupSizes[gi].second / 2.f;
            for (int idx : *groupPtrs[gi])
                sheet.nodes[idx].position.y += yShift;
            yTop += groupSizes[gi].second + typeGroupGap;
        }

        compInfos.push_back({compWidth, totalH});
        compNodes.push_back(nodeIndices);
    }

    // --- 3. Arrange connected components horizontally, centred on x = 0 ---
    float totalWidth = compGap * static_cast<float>(std::max(0, static_cast<int>(compInfos.size()) - 1));
    for (auto &ci : compInfos) totalWidth += ci.width;

    float xLeft = -totalWidth / 2.f;
    for (int ci = 0; ci < static_cast<int>(compInfos.size()); ci++) {
        const float xShift = xLeft + compInfos[ci].width / 2.f;
        for (int idx : compNodes[ci])
            sheet.nodes[idx].position.x += xShift;
        xLeft += compInfos[ci].width + compGap;
    }
}

void DrawConnectionEditor(const UsdStageRefPtr &stage) {
    // We are maintaining a list of graph edit session per stage
    // Each session contains a list of edited node, node visible on the whiteboard
    // Initialization
    static ConnectionsEditorCanvas canvas;
    StageSheets *sheets = editorData.GetSheets(stage);
    if (sheets) {
        if (ImGui::Button(ICON_FA_TRASH)) {
            sheets->DeleteSheet(sheets->GetSelectedSheetID());
        }
        ImGui::SameLine();
        if (ImGui::BeginCombo("##StageSheets", sheets->GetSelectedSheetName().c_str())) {
            for (const auto &sheetName : sheets->GetSheetNames()) {
                if (ImGui::Selectable(sheetName.c_str())) {
                    sheets->SetSelectedSheet(sheetName);
                }
            }
            ImGui::EndCombo();
        }
        
        // Current canvas position, in absolute coordinates.
        //ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();// canvasOrigin, canvasSize
        //ImVec2 canvasSize = ImGui::GetWindowSize();
        // fmodf floating point remainder of the division operation
        // -> so the first line is aligned in the range 0 ___ 1
        ConnectionsSheet &sheet = sheets->GetSelectedSheet();

        // Update the node positions, inputs, outputs`
        // update the connections as well
        sheet.Update();

        ImGui::SameLine();
        static bool pendingFitView = false;
        if (ImGui::Button(ICON_FA_PROJECT_DIAGRAM " Auto Layout")) {
            AutoLayout(sheet);
            pendingFitView = true;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        canvas.Begin(drawList);
        if (pendingFitView) {
            canvas.FitView(sheet);
            pendingFitView = false;
        }
        canvas.DrawGrid();
        canvas.DrawSheet(sheet);
        //canvas.DrawBoundaries(); // for debugging
        //canvas.DrawNodeTest(); // for debugging
        canvas.DrawRegionSelection();
        canvas.UpdateState(); // Might move in End ???
        // canvas.ProcessActions() // TODO ??
        canvas.End();
    }
}

// Add node to blackboard
// New blackboard
// struct ConnectionEditorBlackboard {
//  std::vector<Node> _nodes
//};


/// Create a connection editor session (sheet ???)
void CreateSession(const UsdPrim &prim, const std::vector<UsdPrim> &prims) {
    // Let's create a session
    // Get the blackboard for the stage
    StageSheets *sbb = editorData.GetSheets(prim.GetStage());
    
    if (sbb) {
        // Get the current sheet
        StageSheets::SheetID sheetID = sbb->CreateSheet(prim);
        ConnectionsSheet &bbs = sbb->GetSheet(sheetID);
        bbs.AddNodes(prims);
    }
}


void AddPrimsToCurrentSession(const std::vector<UsdPrim> &prims) {
    // Get the current sheet
    std::vector<UsdPrim> all;
    for (const auto &prim: prims) {
        all.push_back(prim);
        for (const auto &child: prim.GetAllDescendants()) {
            all.push_back(child);
        }
    }
    if (!all.empty()) {
        // Assuming all the prims are coming from the same stage
        StageSheets *sbb = editorData.GetSheets(all[0].GetStage());
        if (sbb) {
            ConnectionsSheet &bbs = sbb->GetSelectedSheet();
            bbs.AddNodes(all);
        }
    }
}


//// Code to look for all the connections in the layer
////std::cout << connections.size() << std::endl;
//auto layer = rootPrim.GetStage()->GetRootLayer();
//std::stack<SdfPath> st;
//st.push(SdfPath::AbsoluteRootPath());
//while (!st.empty()) {
//    const SdfPath path = st.top();
//    st.pop();
//    if (layer->HasField(path, SdfChildrenKeys->PropertyChildren)) {
//        const std::vector<TfToken> &propertyNames =
//            layer->GetFieldAs<std::vector<TfToken>>(path, SdfChildrenKeys->PropertyChildren);
//        for (const TfToken &pName: propertyNames) {
//            const SdfPath propertyPath = path.AppendProperty(pName);
//            if (layer->HasField(propertyPath, SdfChildrenKeys->ConnectionChildren)) {
//                const std::vector<SdfPath> &connectionNames =
//                    layer->GetFieldAs<std::vector<SdfPath>>(path, SdfChildrenKeys->ConnectionChildren);
//                for (const auto &con:connectionNames) {
//                    //std::cout << con.GetText() << std::endl;
//                    connections.emplace_back(path, con);
//                }
//            }
//        }
//    }
//
//
////            const auto &prim = layer->GetPrimAtPath(path);
////            if (prim) {
////                const auto &attributes = prim->GetAttributes();
////                for (const auto &attr:attributes) {
////                    for(const auto &con:attr->GetConnectionPathList().GetAppliedItems()) {
////                        connections.emplace_back(path, con);
////                    }
////                }
////            }
//    static std::vector<TfToken> defaultChildren;
//    //if (layer->HasField(path, SdfChildrenKeys->PrimChildren)) {
//        const std::vector<TfToken> &children =
//        layer->GetFieldAs<std::vector<TfToken>>(path, SdfChildrenKeys->PrimChildren, defaultChildren);
//        for (auto it = children.rbegin(); it != children.rend(); ++it) {
//            st.push(path.AppendChild(*it));
//        }
//    //}
//
//    //if (layer->HasField(path, SdfChildrenKeys->VariantSetChildren)) {
//        const std::vector<TfToken> &variantSetchildren =
//        layer->GetFieldAs<std::vector<TfToken>>(path, SdfChildrenKeys->VariantSetChildren, defaultChildren);
//        // Skip the variantSet paths and show only the variantSetChildren
//        for (auto vSetIt = variantSetchildren.rbegin(); vSetIt != variantSetchildren.rend(); ++vSetIt) {
//            auto variantSetPath = path.AppendVariantSelection(*vSetIt, "");
//
//            //if (layer->HasField(variantSetPath, SdfChildrenKeys->VariantChildren)) {
//                const std::vector<TfToken> &variantChildren =
//                layer->GetFieldAs<std::vector<TfToken>>(variantSetPath, SdfChildrenKeys->VariantChildren, defaultChildren);
//                const std::string &variantSet = variantSetPath.GetVariantSelection().first;
//                for (auto vChildrenIt = variantChildren.rbegin(); vChildrenIt != variantChildren.rend(); ++vChildrenIt) {
//                    st.push(path.AppendVariantSelection(TfToken(variantSet), *vChildrenIt));
//                }
//           // }
//        }
//  //  }
//}
//std::cout << st.size() << " " << connections.size() << std::endl;
