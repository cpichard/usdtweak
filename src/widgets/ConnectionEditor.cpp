
#include "ConnectionEditor.h"
#include "Commands.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include <iostream>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>
#include <stack>

/*
 Notes on the connection editor.
    One approach was to read the whole scene at each frame to deduce/rewrite the graph of connected attributes, however after a
 few tests with the Usd and Sdf api it appears neither of them are fast enough for doing so on a moderate sized scene and low
 performance machine. So we need a different approach, potentially reading only what is needed, ideally without using caching and
 TfNotice We could add "buttons" to look for connections and keep the graph in a local memory
 */

PXR_NAMESPACE_USING_DIRECTIVE

///
///
///             NEW VERSION WORK IN PROGRESS
///
///

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
    UsdPrim prim; // 32 bytes -> might not be a good idea to store that, but let's keep it for now until we know what we need to
                  // display
    SdfPath primPath; // 8 bytes
    // Properties could be tokens or path ?? path could be useful for connecting the properties
    // Copy of the property names to avoid iterating on them (could reduce the name size)
    std::vector<SdfPath> properties;
    bool selected = false;
};

// TODO: what do we need to draw a bezier curve between nodes
// TODO: how is the interaction with bezier curves done ??
struct NodeConnection {
    NodeConnection(const SdfPath &head_, const SdfPath &tail_) : head(head_), tail(tail_) {}
    SdfPath head;
    SdfPath tail; // 8 bytes
};

// TODO black board sheet should also store the canvas position, that would be easier when switching

struct ConnectionsSheet {

    // Should we keep the root prim ?? the root path under which we would create new prims
    ConnectionsSheet() : sheetName("Default") {}
    ConnectionsSheet(const UsdPrim &prim) : rootPrim(prim) {
        sheetName = rootPrim.IsValid() ? rootPrim.GetPrimPath().GetString() : "Default";
    }

    std::vector<UsdPrimNode> nodes;

    // Connections are updated
    std::vector<NodeConnection> connections;

    void AddNodes(const std::vector<UsdPrim> &prims) {
        // Make sure there are no duplicates.
        // We don't want to have the same prim multiple time in the nodes.
        // We want to keep the order in the nodes vector so that we can reference a node
        // by its index
        std::unordered_set<SdfPath, SdfPath::Hash> knownPaths;
        for (const auto &node : nodes) {
            knownPaths.insert(node.primPath);
        }
        for (const UsdPrim &prim : prims) {
            if (knownPaths.find(prim.GetPath()) == knownPaths.end()) {
                nodes.emplace_back(prim);
            }
        }
    }

    UsdStageWeakPtr GetCurrentStage() { return rootPrim.IsValid() ? rootPrim.GetStage() : nullptr; }
    const std::string &GetName() const { return sheetName; }
    // Update the node inputs/outputs positions and the connections such that this is visually
    // coherent
    void Update() {
        // For all the nodes, update positions and connections and attributes ??
        // We need to know the stage
        // static std::vector<std::pair<SdfPath, SdfPath>> connections;
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

        for (auto &node : nodes) {
            // static std::vector<UsdAttribute> properties;
            // properties.clear();
            node.properties.clear();
            // TODO:
            //    Looking at the usd code, GetAuthoredProperties is doing too many things and allocating/release too much memory
            //    This is not going to scale to many nodes for interactive rendering, find a way to get the connections fast
            // const auto properties = node.prim.GetAuthoredProperties();
            for (const UsdAttribute &attr : node.prim.GetAttributes()) {
                // TODO check if it's an input/output or generic connectable parameter
                // node.properties.push_back(attr.GetName().GetString());
                node.properties.push_back(attr.GetPath());
                if (attr.HasAuthoredConnections()) {
                    // properties.push_back(attr);
                    SdfPathVector sources;
                    attr.GetConnections(&sources);
                    for (const auto &path : sources) {
                        // TODO: should we just get the begin and end position of the curve ?
                        // how can we interact with the curve ? to delete connection for example ?
                        connections.emplace_back(attr.GetPath(), path);
                    }
                }
            }
        }
        //@std::cout << "Connections " << connections.size() << std::endl;
    }
    inline void const *GetStageUID() const { return rootPrim.GetStage()->GetUniqueIdentifier(); }

    void GraphLayout() {
        // Attempt to layout the nodes
        // https://github.com/GafferHQ/gaffer/blob/8c18b04e4b3df3d1f9c88c274cddd3db4048b8e9/src/GafferUI/StandardGraphLayout.cpp#L107
        // Testing Sugiyama method
        
        // Build a local graph representation, without taking taking into account the property connections, only using the prim to prim connections
        
        //
        std::unordered_map<SdfPath, int, SdfPath::Hash> pathToNodeId;
        for (int i=0; i < nodes.size(); ++i) {
            const UsdPrimNode &node = nodes[i];
            pathToNodeId.insert({node.primPath, i});
        }
        // Implementation in rust shows that it's not trivial
        // https://github.com/petgraph/petgraph/blob/master/src/algo/feedback_arc_set.rs
        // nodes
        std::vector<std::vector<int>> incoming;
        std::vector<std::vector<int>> outgoing;
        
        for (const auto &con : connections) {
            int headIndex = pathToNodeId[con.head.GetPrimPath()];
            int tailIndex = pathToNodeId[con.tail.GetPrimPath()];
            incoming[headIndex].push_back(tailIndex);
            outgoing[tailIndex].push_back(headIndex);
        }
        // Need incoming and outgoing lists
        std::vector<int> incomingSize;
        std::vector<int> outgoingSize;
        for (int nodeId = 0; nodeId < outgoingSize.size(); ++nodeId) {
            auto &lst = outgoing[nodeId];
            std::sort(lst.begin(), lst.end());
            lst.erase(std::unique(lst.begin(), lst.end()), lst.end());
            
        }
        for (int nodeId = 0; nodeId < incoming.size(); ++nodeId) {
            auto &lst = incoming[nodeId];
            std::sort(lst.begin(), lst.end());
            lst.erase(std::unique(lst.begin(), lst.end()), lst.end());
        }
        
        // Remove cycles with a greedy cycle removal method
        std::vector<bool> G(incoming.size(), true);
        std::vector<int> Sr;
        std::vector<int> Sl;
        bool remainingNode = false;

        while (remainingNode) {
            // Looks Sinks
            for (int nodeId = 0; nodeId < G.size(); ++nodeId) {
                if (G[nodeId]) {
                    remainingNode = true;
                    if (incomingSize[nodeId] >= 0
                        && outgoingSize[nodeId] == 0) {
                        G[nodeId] = false;
                        // TODO remove links
                        // prepend to sr
                        Sr.push_back(nodeId);
                    }
                }
            }
            // Looks for Source

        }
        
        
        
        
        
        
        
    }
    
    
    // root prim defines the stage and the root prim to add other prim under
    UsdPrim rootPrim;
    std::string sheetName;
};

// This might end up as an exposed class, but for now we keep it hidden in the translation unit
struct NodeConnectionEditorData {

    NodeConnectionEditorData() {}

    std::vector<ConnectionsSheet> sheets; // TODO as pointer

    void SetCurrentStage(const UsdStageRefPtr &stage) {
        const auto &found = selectedSheetPerStage.find(stage->GetUniqueIdentifier());
        if (found == selectedSheetPerStage.end()) {
            SetSelectedSheet(-1);
        } else {
            SetSelectedSheet(found->second);
        }
    }

    ConnectionsSheet *GetSelectedSheet() { return (selectedSheet >= 0) ? &sheets[selectedSheet] : nullptr; }

    std::vector<ConnectionsSheet> &GetSheets() { return sheets; }

    void SetSelectedSheet(int sheetID) {
        selectedSheet = sheetID;
        if (sheetID >= 0) {
            selectedSheetPerStage[sheets[sheetID].GetStageUID()] = sheetID;
        }
    }

    void DeleteSelectedSheet() {
        // TODO a command !!!
    }

    int AddSheet(const UsdPrim &prim) {
        sheets.push_back(ConnectionsSheet(prim));
        return static_cast<int>(sheets.size()) - 1;
    }

    void AddPrimsToSelectedSheet(const std::vector<UsdPrim> &prims) {
        if (selectedSheet >= 0) {
            sheets[selectedSheet].AddNodes(prims);
            sheets[selectedSheet].GraphLayout();
        }
    }

    //    StageSheets * GetSheets(UsdStageWeakPtr stage) {
    //        if (stage) {
    //            if (stageSheets.find(stage->GetUniqueIdentifier()) == stageSheets.end()) {
    //                stageSheets.insert({stage->GetUniqueIdentifier(), StageSheets(stage)});
    //            }
    //            return & stageSheets.at(stage->GetUniqueIdentifier());
    //        }
    //        return nullptr;
    //    }

    // Not even sure that shouldn't go up in the hierachy, belong to the caller
    std::unordered_map<const void *, int> selectedSheetPerStage;
    int selectedSheet = -1;
};

// global variable ...
static NodeConnectionEditorData editorData;

// TODO create manipulators for editing node positions, selections, etc
// TODO we can also inherit the canvas to reuse the bits like scrolling, zooming, etc
struct ConnectionsEditorCanvas { // rename to InfiniteCanvas ??

    // Testing with event and state machine
    uint8_t state = 0; // current edition state of the canvas, (selecting, moving node, zoom/pan?)
    uint8_t event = 0; // events on the current rendered frame
    uint8_t previousState = 0;
    // ??? events we need
    enum Events : uint8_t {
        IDLE = 0,
        CANVAS_CLICKED,
        CANVAS_CLICKED_PANNING,
        CANVAS_CLICKED_ZOOMING,
        NODE_CLICKED,
        CONNECTOR_CLICKED,
        CLICK_RELEASED
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

    void Begin(ImDrawList *drawList_) {

        // Reset state
        event = Events::IDLE;     // reset event
        hasSelectedNodes = false; // reset selected nodes

        drawList = drawList_;
        ImGuiContext &g = *GImGui;
        // Current widget position, in absolute coordinates. (relative to the main window)
        widgetOrigin = ImGui::GetCursorScreenPos(); // canvasOrigin, canvasSize
        // WindowSize returns the size of the whole window, including tabs that we have to remove to get the widget size.
        // GetCursorPos gives the current position in window coordinates
        widgetSize = ImGui::GetWindowSize() - ImGui::GetCursorPos() - g.Style.WindowPadding; // Should also add the borders
        originOffset = (widgetSize / 2.f);                                                   // in window Coordinates
        drawList->ChannelsSplit(2);                                                          // Foreground and background
        // TODO: we might want to use only widgetBB and get rid of widgetOrigin and widgetSize
        widgetBoundingBox.Min = widgetOrigin;
        widgetBoundingBox.Max = widgetOrigin + widgetSize;
        drawList->PushClipRect(widgetBoundingBox.Min, widgetBoundingBox.Max);
        // There is no overlapping of widgets unfortunateluy
        // if (ImGui::InvisibleButton("canvas", widgetBoundingBox.GetSize())) {
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
                    ImGuiIO &io = ImGui::GetIO();
                    zoomClick = io.MouseClickedPos[ImGuiMouseButton_Right];
                }
            } else if (ImGui::IsMouseReleased(0) || ImGui::IsMouseReleased(1)) { // TODO Should be any button ??
                event = Events::CLICK_RELEASED;
            }
        } else {
            event = Events::CLICK_RELEASED;
        }
    }

    // What we call canvas is the infinite normalized region
    inline ImVec2 CanvasToWindow(const ImVec2 &posInCanvas) { return posInCanvas * zooming + scrolling + originOffset; }

    inline ImVec2 WindowToCanvas(const ImVec2 &posInWindow) { return (posInWindow - scrolling - originOffset) / zooming; }

    inline ImVec2 WindowToScreen(const ImVec2 &posInWindow) { return posInWindow + widgetOrigin; }

    inline ImVec2 ScreenToWindow(const ImVec2 &posInScreen) { return posInScreen - widgetOrigin; }

    // The imgui draw functions are expressed in absolute screen coordinates.
    inline ImVec2 CanvasToScreen(const ImVec2 &posInCanvas) { return WindowToScreen(CanvasToWindow(posInCanvas)); }

    inline ImVec2 ScreenToCanvas(const ImVec2 &posInScreen) { return WindowToCanvas(ScreenToWindow(posInScreen)); }

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
        gridOrigin = ImVec2(ceilf(gridOrigin.x / gridSpacing) * gridSpacing, ceilf(gridOrigin.y / gridSpacing) * gridSpacing);
        gridOrigin = CanvasToScreen(gridOrigin);
        auto gridSize0 = CanvasToScreen(ImVec2(0.f, 0.f));
        auto gridSize1 = CanvasToScreen(ImVec2(gridSpacing, gridSpacing));
        auto gridSize = gridSize1 - gridSize0;

        // Draw in screen space
        for (float x = gridOrigin.x; x < widgetOrigin.x + widgetSize.x; x += gridSize.x) {
            drawList->AddLine(ImVec2(x, widgetOrigin.y), ImVec2(x, widgetOrigin.y + widgetSize.y), gridColor);
        }
        for (float y = gridOrigin.y; y < widgetOrigin.y + widgetSize.y; y += gridSize.y) {
            drawList->AddLine(ImVec2(0.f, y), ImVec2(widgetOrigin.x + widgetSize.x, y), gridColor);
        }
    }

    void DrawNode(UsdPrimNode &node) {
        ImGuiContext &g = *GImGui;
        ImGuiIO &io = ImGui::GetIO();
        drawList->ChannelsSetCurrent(1); // Foreground
        constexpr float headerHeight = 50.f;
        constexpr float connectorSize = 14.f; // square connector size

        // static std::vector<UsdAttribute> properties;
        // properties.clear();
        const std::vector<SdfPath> &properties = node.properties;
        // TODO:
        //    Looking at the usd code, GetAuthoredProperties is doing too many things and allocating/release to much memory
        //    This is not going to scale to many nodes, find a way to get the connections fast
        // const auto properties = node.prim.GetAuthoredProperties();
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
        float nodeHeight = (nbNodes + 2) * (g.FontSize + 2.f) + headerHeight; // 50 == header size
        ImVec2 nodeMin = ImVec2(-80.f, -nodeHeight / 2.f) + node.position;
        ImVec2 nodeMax = ImVec2(80.f, nodeHeight / 2.f) + node.position;
        // The invisible button will trigger the sliders if it's not clipped
        ImRect nodeBoundingBox(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax));
        nodeBoundingBox.ClipWith(widgetBoundingBox);

        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), 0xFF090920, 4.0f);
        drawList->AddRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax),
                          node.selected ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 255, 255), 4.0f);

        // TODO: add padding, truncate name if too long, add tooltip
        drawList->AddText(g.Font, g.FontSize * zooming, CanvasToScreen(nodeMin), IM_COL32(255, 255, 255, 255),
                          node.prim.GetName().GetText());

        // Check if the user clicked on the node and update the event.
        // The event might be again updated later on, on the connectors as well, that's how choosing the event is implemented
        if (ImGui::IsMouseClicked(0)) {
            if (nodeBoundingBox.Contains(ImGui::GetMousePos())) {
                event = Events::NODE_CLICKED; // Node clicked
                nodeClicked = &node;
            }
        }

        // Draw properties
        const ImVec2 inputStartPos = nodeMin + ImVec2(10.f, headerHeight);
        for (int i = 0; i < properties.size(); i++) {
            const float linePos = i * (g.FontSize + 2.f); // 2.f == padding

            // Input connector position min max
            const auto conMin = ImVec2(nodeMin.x - connectorSize / 2, inputStartPos.y + 1.f + linePos);
            const auto conMax = ImVec2(nodeMin.x + connectorSize / 2, inputStartPos.y + 15.f + linePos);
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
            connectorColor = connectorTailClicked == properties[i] ? IM_COL32(255, 127, 127, 255) : IM_COL32(255, 255, 255, 255);
            const auto outConMin = ImVec2(nodeMax.x - connectorSize / 2, inputStartPos.y + 1.f + linePos);
            const auto outConMax = ImVec2(nodeMax.x + connectorSize / 2, inputStartPos.y + 15.f + linePos);
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
            // propertie[i].
            drawList->AddText(g.Font, g.FontSize * zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255),
                              properties[i].GetNameToken().GetText());

            // Update positions of the connectors
            connectorPositions[properties[i]] =
                ImVec4(nodeMin.x, inputStartPos.y + 9.f + linePos, nodeMax.x, inputStartPos.y + 9.f + linePos);
        }

        // TODO: will we need invisible buttons ? code left here
        // ImGui::SetCursorScreenPos(nodeBoundingBox.Min);
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
                ImGuiIO &io = ImGui::GetIO();
                node.position = node.position + io.MouseDelta / zooming;
            } else if (!nodeClicked->selected && nodeClicked == &node) {
                // TODO this could be a function UpdateNodePosition(node)
                nodeClicked->position = nodeClicked->position + io.MouseDelta / zooming;
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
    //            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255),
    //            outputs[i]);
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
        ImGuiContext &g = *GImGui;
        ImGuiIO &io = ImGui::GetIO();
        currentStage = sheet.GetCurrentStage();
        connectorPositions.clear();
        // ImGuiWindow* window = g.CurrentWindow;
        // Give the node a position in canvas coordinates
        drawList->ChannelsSetCurrent(1); // Foreground
        // static ImVec2 nodePos(0.f, 0.f); // Position in the canvas
        for (auto &node : sheet.nodes) {
            DrawNode(node);
        }
        drawList->ChannelsSetCurrent(0); // Background
        for (const auto &con : sheet.connections) {
            const auto arrowHead = connectorPositions.find(con.head);
            if (arrowHead != connectorPositions.end()) {
                const auto arrowTail = connectorPositions.find(con.tail);
                if (arrowTail != connectorPositions.end()) {
                    ImVec2 p2a(arrowHead->second.x, arrowHead->second.y);
                    ImVec2 p1a(arrowTail->second.z, arrowTail->second.w);
                    ImVec2 p2b(arrowHead->second.x - 150, arrowHead->second.y);
                    ImVec2 p1b(arrowTail->second.z + 150, arrowTail->second.w);
                    ImVec2 mouse = ImGui::GetMousePos();
                    auto color = IM_COL32(255, 255, 255, 255);
                    // ImVec2 closest = ImBezierCubicClosestPointCasteljau(CanvasToScreen(p1a), CanvasToScreen(p1b),
                    // CanvasToScreen(p2b), CanvasToScreen(p2a), mouse, O.2);
                    drawList->AddBezierCubic(CanvasToScreen(p1a), CanvasToScreen(p1b), CanvasToScreen(p2b), CanvasToScreen(p2a),
                                             color, 2);
                }
            }
        }

        // Show connecting node
        if (state == CONNECTING_NODES) {
            const auto arrowTail = connectorPositions.find(connectorTailClicked);
            ImVec2 p1(arrowTail->second.z, arrowTail->second.w);
            drawList->AddLine(CanvasToScreen(p1), ImGui::GetMousePos(), IM_COL32(255, 127, 127, 255));
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
    // Not very readable, but it kind of works.
    // In this function only states and events are processed
    // Actions on state changes are done in ProcessChange
    void UpdateState() {
        ImGuiIO &io = ImGui::GetIO();
        previousState = state;
        if (state == States::HOVERING_CANVAS) {
            if (event == NODE_CLICKED) {
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
            }
        } else if (state == SELECTING_NODE) {
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
                // TODO PROCESS SELECTED NODE
                // SELECT / DESELECT NODE
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f)) {
                state = MOVING_NODE;
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
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
                ZoomFromPosition(zoomClick, io.MouseDelta);
            }
        } else if (state == MOVING_NODE) {
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
                nodeClicked = nullptr; // TODO move to End();
            }
        } else if (state == CONNECTING_NODES) {
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
            }
        }
        // Debug
        ImGuiContext &g = *GImGui;
        std::string stateDebug = "State " + std::to_string(state);
        drawList->AddText(g.Font, g.FontSize * zooming, CanvasToScreen(ImVec2(0.2, 0.2)), IM_COL32(255, 255, 255, 255),
                          stateDebug.c_str());
    }

    // Process state change /
    void ProcessChanges(ConnectionsSheet &sheet) {
        // NODE_SELECTION and RELEASED : action select node, should we add actions for selections ??
        // We certainly want actions for linking nodes
        static std::vector<ImVec2> positions;
        if (previousState != state) {
            // State change
            if (previousState == SELECTING_NODE && state == MOVING_NODE) {
                std::cout << "STORE POSITION for UNDO/REDO" << std::endl;
                for (const auto &node : sheet.nodes) {
                    if (node.selected) {
                        positions.push_back(node.position);
                    }
                }
            }
            if (previousState == MOVING_NODE && state == HOVERING_CANVAS) {
                std::cout << "Add Command" << std::endl;
                std::vector<ImVec2>::const_iterator it = positions.begin();
                for (const auto &node : sheet.nodes) {
                    if (node.selected) {
                        std::cout << it->x << " " << it->y << " -> " << node.position.x << " " << node.position.y << std::endl;
                        it++;
                    }
                }
                positions.clear();
            }
        }
        if (previousState == CONNECTING_NODES && state == HOVERING_CANVAS) {
            if (connectorHeadClicked != SdfPath::EmptyPath()) {
                // Launch a command !
                ExecuteAfterDraw<AttributeConnect>(currentStage, connectorTailClicked, connectorHeadClicked);
            }
        }
    }

    // Clean local variable for the next frame
    void End() {
        if (previousState == CONNECTING_NODES && state == HOVERING_CANVAS) {
            // This could go in process changes instead ??
            connectorTailClicked = SdfPath::EmptyPath();
            connectorHeadClicked = SdfPath::EmptyPath();
        }

        if (previousState == MOVING_NODE && state == HOVERING_CANVAS) {
            // This could go in process changes instead ??
            nodeClicked = nullptr;
        }

        drawList->ChannelsMerge();
        drawList->PopClipRect();
    };

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
    float zooming = 1.f;                      // TODO: make sure zooming is never 0
    ImVec2 zoomClick = ImVec2(0.0f, 0.0f);    // Zoom origin
    ImVec2 selectionOrigin;                   // TODO this could be union with zoom click (origin)
    ImVec2 widgetOrigin = ImVec2(0.0f, 0.0f); // canvasOrigin, canvasSize in screen coordinates
    ImVec2 widgetSize = ImVec2(0.0f, 0.0f);
    ImRect widgetBoundingBox;

    // Should that go in the inherited class ?
    UsdPrimNode *nodeClicked = nullptr;

    SdfPath connectorTailClicked;
    SdfPath connectorHeadClicked;

    // Connectors positions
    std::unordered_map<SdfPath, ImVec4, SdfPath::Hash> connectorPositions; // 2 in 2 out
    // std::unordered_map<SdfPath, ImVec2, SdfPath::Hash> outputsPositions;

    bool hasSelectedNodes = false; // computed at each frame

    ImDrawList *drawList = nullptr;

    UsdStageWeakPtr currentStage;
};

static ConnectionsEditorCanvas canvas;

ConnectionsEditorCanvas &GetConnectionEditor() { return canvas; };

void DrawConnectionEditor(const UsdStageRefPtr &stage) {
    // We are maintaining a list of graph edit session per stage
    // Each session contains a list of edited node, node visible on the whiteboard
    // Initialization

    // StageSheets *sheets = editorData.GetSheets(stage);
    // if (sheets) {

    // The stage might change, in that case the selected sheet will also change
    editorData.SetCurrentStage(stage);

    ConnectionsSheet *selectedSheet = editorData.GetSelectedSheet();
    if (selectedSheet && stage) {
        if (ImGui::Button(ICON_FA_TRASH)) {
            editorData.DeleteSelectedSheet();
        }
        ImGui::SameLine();
        if (ImGui::BeginCombo("##StageSheets", selectedSheet->GetName().c_str())) {
            int sheetId = 0;
            for (const auto &sheet : editorData.GetSheets()) {
                // std::cout <<sheet.GetStageUID() << std::endl;

                std::cout << sheet.rootPrim.GetStage()->GetUniqueIdentifier() << std::endl;
                std::cout << sheet.rootPrim.GetStage().GetUniqueIdentifier() << std::endl;
                // std::cout << sheet.rootPrim.GetStage()->;
                std::cout << stage->GetUniqueIdentifier() << std::endl;
                std::cout << "------------" << std::endl;
                if (sheet.GetStageUID() == stage->GetUniqueIdentifier()) {
                    if (ImGui::Selectable(sheet.GetName().c_str())) {
                        editorData.SetSelectedSheet(sheetId);
                    }
                }
                sheetId++;
            }
            ImGui::EndCombo();
        }

        // Current canvas position, in absolute coordinates.
        // ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();// canvasOrigin, canvasSize
        // ImVec2 canvasSize = ImGui::GetWindowSize();
        // fmodf floating point remainder of the division operation
        // -> so the first line is aligned in the range 0 ___ 1

        // Update the node positions, inputs, outputs`
        // update the connections as well
        selectedSheet->Update();

        ImDrawList *drawList = ImGui::GetWindowDrawList();

        // TODO canvas could be polymorphic in the end,
        // all the functions with sheet could be inherited
        canvas.Begin(drawList);
        canvas.DrawGrid();
        canvas.DrawSheet(*selectedSheet);
        // canvas.DrawBoundaries(); // for debugging
        // canvas.DrawNodeTest(); // for debugging
        canvas.DrawRegionSelection();
        canvas.UpdateState();                  // Might move in End ???
        canvas.ProcessChanges(*selectedSheet); // TODO ??
        canvas.End();
    }
}


/// Create a connection editor session (sheet ???)
void ConnectionEditorCreateSheet(const UsdPrim &prim, const std::vector<UsdPrim> &prims) {
    int sheetID = editorData.AddSheet(prim);
    editorData.SetSelectedSheet(sheetID);
    editorData.AddPrimsToSelectedSheet(prims);
}

void ConnectionEditorAddPrims(const std::vector<UsdPrim> &prims) {
    // Get the current sheet
    editorData.AddPrimsToSelectedSheet(prims);
}

//// Code to look for all the connections in the layer
////std::cout << connections.size() << std::endl;
// auto layer = rootPrim.GetStage()->GetRootLayer();
// std::stack<SdfPath> st;
// st.push(SdfPath::AbsoluteRootPath());
// while (!st.empty()) {
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
// std::cout << st.size() << " " << connections.size() << std::endl;

void ConnectionEditorMoveNodes(SheetID sheetId, const std::unordered_map<SdfPath, ImVec2, SdfPath::Hash> &newPositions) {

    //    StageSheets *sheets = editorData.GetSheets();
    //    if (sheets){
    //        ConnectionsSheet &sheet = sheets->GetSheet(sheetId);
    //        for (auto &node:sheet.nodes) {
    //            auto it = newPositions.find(node.primPath);
    //            if (it!=newPositions.end()){
    //                node.position = it->second;
    //            }
    //        }
    //    }
    //
    //
    //
}


