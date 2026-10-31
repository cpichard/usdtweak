
#include "ConnectionEditor.h"
#include "Editor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "Commands.h"
#include "ShaderDefinition.h"
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
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
        UsdUINodeGraphNodeAPI api(prim_);
        GfVec2f usdPos;
        if (api.GetPosAttr().Get(&usdPos)) {
            position = ImVec2(usdPos[0], usdPos[1]);
            hasUsdPosition = true;
        }
    }

    ImVec2 position; // 8 bytes
    bool hasUsdPosition = false; // the position was read from ui:nodegraph:node:pos
    UsdPrim prim; // 32 bytes -> might not be a good idea to store that, but let's keep it for now until we know what we need to display
    SdfPath primPath; // 8 bytes
    // Properties could be tokens or path ?? path could be useful for connecting the properties
    // Copy of the property names to avoid iterating on them (could reduce the name size)
    std::vector<SdfPath> properties;
    // Same size as properties, true when the property is shown because it's authored rather than connected
    std::vector<bool> authoredProperties;
    bool selected = false;
};

// TODO: what do we need to draw a bezier curve between nodes
// TODO: how is the interaction with bezier curves done ??
struct NodeConnection {
    NodeConnection(const SdfPath &begin_, const SdfPath &end_) : begin(begin_), end(end_) {}
    SdfPath begin;
    SdfPath end;     // 8 bytes
};

static bool IsUiNodeGraphProperty(const UsdAttribute &attr) {
    return TfStringStartsWith(attr.GetName().GetString(), "ui:nodegraph:");
}

// An attribute which can be connected but isn't necessarily on the prim yet. When it's not, typeToCreate is the
// type used to create it, otherwise it is empty.
struct ConnectableAttribute {
    TfToken name;
    SdfValueTypeName typeToCreate;
};

// Inputs and outputs declared by the shader definition (Sdr) of a UsdShade shader. They are usually not authored
// on the prim and UsdPrim::GetAttributes doesn't return them.
static void AppendShaderDefinitionAttributes(const UsdPrim &prim, std::vector<ConnectableAttribute> &attributes) {
    SdrShaderNodeConstPtr shaderNode = GetShaderDefinition(prim);
    if (!shaderNode)
        return;
    auto appendProperties = [&](const auto &names, const std::string &prefix, auto getProperty) {
        for (const TfToken &name : names) {
            if (SdrShaderPropertyConstPtr property = getProperty(name)) {
                attributes.push_back({TfToken(prefix + name.GetString()), GetShaderPropertySdfType(property)});
            }
        }
    };
    appendProperties(GetShaderDefinitionInputNames(shaderNode), "inputs:",
                     [&](const TfToken &name) { return shaderNode->GetShaderInput(name); });
    appendProperties(GetShaderDefinitionOutputNames(shaderNode), "outputs:",
                     [&](const TfToken &name) { return shaderNode->GetShaderOutput(name); });
}

// Attributes which can be connected on a prim: the existing ones followed by the ones declared by its shader
// definition or, for a material, its universal terminal outputs.
static std::vector<ConnectableAttribute> GetConnectableAttributes(const UsdPrim &prim, bool showUiNodeGraphProperties) {
    std::vector<ConnectableAttribute> attributes;
    for (const UsdAttribute &attr : prim.GetAttributes()) {
        if (!showUiNodeGraphProperties && IsUiNodeGraphProperty(attr))
            continue;
        attributes.push_back({attr.GetName(), SdfValueTypeName()});
    }
    std::vector<ConnectableAttribute> declared;
    AppendShaderDefinitionAttributes(prim, declared);
    if (UsdShadeMaterial(prim)) {
        for (const TfToken &output : {UsdShadeTokens->surface, UsdShadeTokens->displacement, UsdShadeTokens->volume}) {
            declared.push_back({TfToken("outputs:" + output.GetString()), SdfValueTypeNames->Token});
        }
    }
    for (const ConnectableAttribute &attribute : declared) {
        if (!prim.HasAttribute(attribute.name))
            attributes.push_back(attribute);
    }
    return attributes;
}

// Connect source to destination, creating them first when their type is given and they don't exist.
// The edits are recorded in a single undoable command.
static void ConnectAttributes(const UsdStageWeakPtr &stage, const SdfPath &source, const SdfValueTypeName &sourceType,
                              const SdfPath &destination, const SdfValueTypeName &destinationType) {
    if (!stage)
        return;
    std::function<void()> connect = [=]() {
        if (!stage)
            return;
        auto getOrCreateAttribute = [&](const SdfPath &path, const SdfValueTypeName &type) {
            UsdAttribute attr = stage->GetAttributeAtPath(path);
            if (!attr && type) {
                if (UsdPrim prim = stage->GetPrimAtPath(path.GetPrimPath())) {
                    attr = prim.CreateAttribute(path.GetNameToken(), type, false /* custom */);
                }
            }
            return attr;
        };
        getOrCreateAttribute(source, sourceType);
        if (UsdAttribute destinationAttr = getOrCreateAttribute(destination, destinationType)) {
            destinationAttr.AddConnection(source);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(TfCreateRefPtrFromProtectedWeakPtr(stage->GetEditTarget().GetLayer()), connect);
}

// Materials and NodeGraphs, they can be opened in their own container sheet
static bool IsContainerPrim(const UsdPrim &prim) {
    return prim && prim.IsA<UsdShadeNodeGraph>(); // Material derives from NodeGraph
}

struct ConnectionsSheet {

    // Should we keep the root prim ?? the root path under which we would create new prims
    // TODO we can have a default sheet without a stage
    ConnectionsSheet() {}
    // A container sheet shows the root prim and all its children, removing a node deletes its prim.
    // The other sheets show the prims added by the user, removing a node only removes it from the sheet.
    ConnectionsSheet(const UsdPrim &prim, bool isContainer_)
        : rootPrim(prim), rootPath(prim ? prim.GetPath() : SdfPath()), showAllParameters(!isContainer_),
          isContainer(isContainer_) {}

    std::vector<UsdPrimNode> nodes;
    
    // Connections are updated
    std::vector<NodeConnection> connections;
    
    void AddNodes(const std::vector<UsdPrim> &prims) {
        // Skip prims that are already present so we never show the same prim twice.
        std::unordered_set<SdfPath, SdfPath::Hash> existing;
        existing.reserve(nodes.size());
        for (const UsdPrimNode &node : nodes)
            existing.insert(node.primPath);
        for (const UsdPrim &prim:prims) {
            if (prim && existing.insert(prim.GetPath()).second) {
                nodes.emplace_back(prim);
            }
        }
    }
    
    bool IsRootNode(const UsdPrimNode &node) const { return isContainer && node.primPath == rootPath; }

    // The prims of instances and prototypes can't be edited, the children of an instance are instance proxies
    bool IsReadOnly() const {
        return isContainer && rootPrim.IsValid() &&
               (rootPrim.IsInstance() || rootPrim.IsInstanceProxy() || rootPrim.IsInPrototype());
    }

    // The nodes of a container sheet follow the children of the root prim, the nodes already in the sheet are kept
    // with their positions and selection.
    void UpdateContainerNodes(const UsdStageWeakPtr &stage) {
        if (!rootPrim.IsValid()) {
            rootPrim = stage ? stage->GetPrimAtPath(rootPath) : UsdPrim();
            if (!rootPrim.IsValid()) {
                nodes.clear(); // their prims might be expired as well, the sheet is deleted by StageSheets
                return;
            }
        }
        std::vector<UsdPrim> prims{rootPrim};
        for (const UsdPrim &child : rootPrim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
            prims.push_back(child);
        }
        const bool unchanged = prims.size() == nodes.size() &&
                               std::equal(prims.begin(), prims.end(), nodes.begin(),
                                          [](const UsdPrim &prim, const UsdPrimNode &node) { return prim.GetPath() == node.primPath; });
        if (unchanged) {
            for (size_t i = 0; i < nodes.size(); i++) {
                nodes[i].prim = prims[i]; // also refreshes the expired handles
            }
            return;
        }
        std::unordered_map<SdfPath, size_t, SdfPath::Hash> existing;
        for (size_t i = 0; i < nodes.size(); i++) {
            existing[nodes[i].primPath] = i;
        }
        std::vector<UsdPrimNode> updatedNodes;
        updatedNodes.reserve(prims.size());
        for (const UsdPrim &prim : prims) {
            auto nodeIt = existing.find(prim.GetPath());
            if (nodeIt != existing.end()) {
                updatedNodes.push_back(std::move(nodes[nodeIt->second]));
                updatedNodes.back().prim = prim;
            } else {
                updatedNodes.emplace_back(prim);
            }
        }
        nodes = std::move(updatedNodes);
    }

    // Update the node inputs/outputs positions and the connections such that this is visually
    // coherent
    void Update(const UsdStageWeakPtr &stage) {
        // The stored UsdPrim handles expire when their prim is removed from the stage (deleted, renamed,
        // deactivated or when an instance prototype is rebuilt). Calling methods on an expired prim crashes,
        // so we re-resolve them by path and drop the nodes that no longer exist.
        // This is done here rather than with TfNotice as the ObjectsChanged notice doesn't report all the
        // expired prims, for example instance proxies only get their prototype path resynced.
        // The container sheets resolve their nodes from the children of the root prim instead.
        if (isContainer) {
            UpdateContainerNodes(stage);
        } else {
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                       [&](UsdPrimNode &node) {
                                           if (!node.prim.IsValid()) {
                                               node.prim = stage ? stage->GetPrimAtPath(node.primPath) : UsdPrim();
                                           }
                                           return !node.prim.IsValid();
                                       }),
                        nodes.end());
        }

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
            node.authoredProperties.clear();
            // TODO:
            //    Looking at the usd code, GetAuthoredProperties is doing too many things and allocating/release too much memory
            //    This is not going to scale to many nodes for interactive rendering, find a way to get the connections fast
            //const auto properties = node.prim.GetAuthoredProperties();
            for (const UsdAttribute &attr: node.prim.GetAttributes()) {
                if (!showAllParameters && IsUiNodeGraphProperty(attr)) {
                    continue;
                }
                // TODO check if it's an input/output or generic connectable parameter
                //node.properties.push_back(attr.GetName().GetString());
                node.properties.push_back(attr.GetPath());
                node.authoredProperties.push_back(!showAllParameters && attr.HasAuthoredValue()
                                                  && !TfStringStartsWith(attr.GetName().GetString(), "info:"));
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

        // Unless all the parameters are shown, keep the properties at either end of a connection and the authored ones.
        // The others are reachable with the "+" row.
        if (!showAllParameters) {
            std::unordered_set<SdfPath, SdfPath::Hash> connected;
            for (const NodeConnection &connection : connections) {
                connected.insert(connection.begin);
                connected.insert(connection.end);
            }
            for (auto &node : nodes) {
                size_t kept = 0;
                for (size_t i = 0; i < node.properties.size(); i++) {
                    const bool isConnected = connected.count(node.properties[i]) != 0;
                    if (isConnected || node.authoredProperties[i]) {
                        node.properties[kept] = node.properties[i];
                        node.authoredProperties[kept] = !isConnected && node.authoredProperties[i];
                        kept++;
                    }
                }
                node.properties.resize(kept);
                node.authoredProperties.resize(kept);
            }
        }
    }

    // root prim to know the stage and the prim to add other prim under
    UsdPrim rootPrim;
    SdfPath rootPath; // to resolve rootPrim again when it expires
    // All the attributes, ui:nodegraph ones included, otherwise only the connected and authored ones
    // and a "+" row to connect the others. Off by default on the container sheets.
    bool showAllParameters = true;
    bool isContainer = false;
    bool needsInitialLayout = false; // laid out and framed on its first draw

    // Canvas view of the sheet, loaded in the canvas when the sheet is drawn and stored back after,
    // so each sheet keeps its own pan and zoom
    ImVec2 scrolling = ImVec2(0.0f, 0.0f);
    float zooming = 1.f;
};

static const std::string defaultSheetID("default");

// One set of sheets per stage.
// Each sheets contains nodes which are representations of prims
struct StageSheets {

    using SheetID = std::string; // TODO might be more efficient with int
    StageSheets() = delete;
    StageSheets(UsdStageWeakPtr stage) {
        // We have a default empty sheet which is linked to the root
        // make sense to call it default ??
        if (stage) {
            selectedSheet = defaultSheetID;
            sheets[selectedSheet] = ConnectionsSheet(stage->GetDefaultPrim(), false);
            sheetNames.push_back(selectedSheet);
        }
    }

    // We expect to have max around 20 sheets, that would be a lot already
    std::unordered_map<SheetID, ConnectionsSheet> sheets;
    SheetID selectedSheet;
    std::vector<std::string> sheetNames;
    
    // Create the container sheet of rootPrim if it doesn't exist yet. The sheets are keyed by the root prim path,
    // so renaming the root prim closes its sheet.
    SheetID CreateContainerSheet(const UsdPrim &rootPrim) {
        SheetID sheetID = rootPrim.GetPath().GetString();
        auto sheetIt = sheets.find(sheetID);
        if (sheetIt == sheets.end()) {
            sheetNames.push_back(sheetID);
            ConnectionsSheet &sheet = sheets[sheetID] = ConnectionsSheet(rootPrim, true);
            sheet.needsInitialLayout = true;
        }
        return sheetID;
    }

    // Close the container sheets whose root prim was removed from the stage (deleted, renamed, deactivated, ...)
    void DeleteExpiredContainerSheets(const UsdStageWeakPtr &stage) {
        std::vector<SheetID> expired;
        for (const auto &sheetID : sheetNames) {
            const ConnectionsSheet &sheet = sheets[sheetID];
            if (!sheet.isContainer)
                continue;
            // A deactivated prim is still a valid prim
            const UsdPrim rootPrim =
                sheet.rootPrim.IsValid() ? sheet.rootPrim : (stage ? stage->GetPrimAtPath(sheet.rootPath) : UsdPrim());
            if (!rootPrim || !rootPrim.IsActive()) {
                expired.push_back(sheetID);
            }
        }
        for (const auto &sheetID : expired) {
            DeleteSheet(sheetID);
        }
    }
    
    // The default sheet can't be deleted. When the selected sheet is deleted, its left neighbour is selected.
    void DeleteSheet(const SheetID &sheetID) {
        if (sheetID == defaultSheetID)
            return;
        auto nameIt = std::find(sheetNames.begin(), sheetNames.end(), sheetID);
        if (nameIt == sheetNames.end())
            return;
        if (selectedSheet == sheetID) {
            selectedSheet = nameIt == sheetNames.begin() ? defaultSheetID : *std::prev(nameIt);
        }
        sheetNames.erase(nameIt);
        sheets.erase(sheetID);
    }

    bool IsDefaultSheet(const SheetID &sheetID) const { return sheetID == defaultSheetID; }

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
    
    // Sheets in tab order
    const std::vector<std::string> & GetSheetNames() const {
        return sheetNames;
    }
};


// This might end up as an exposed class, but for now we keep it hidden in the translation unit
struct NodeConnectionEditorData {

    // Storing the unique identifier as a key won't keep the data if we unload and reload the stage
    // with undo/redo
    std::unordered_map<void const *, StageSheets> stageSheets;

    // A sheet was opened, potentially from another window while this one is hidden
    bool focusRequested = false;

    StageSheets * GetSheets(UsdStageWeakPtr stage) {
        if (stage) {
            auto id = stage->GetUniqueIdentifier();
            if (stageSheets.find(id) == stageSheets.end()) {
                stageSheets.insert({id, StageSheets(stage)});
            }
            return &stageSheets.at(id);
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
        CONNECTION_CLICKED,
        CONNECT_ROW_CLICKED, // "+" row of a node
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
        openNodeContextMenu = false; // reset right-click menu request
        openAttributeMenu = false; // reset "+" row menu request
        nodeDoubleClickedPath = SdfPath::EmptyPath();

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
        // While a popup (e.g. the node context menu) is open, the canvas must ignore
        // clicks — otherwise clicking a menu item also starts a region selection.
        const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        // During our own pan/zoom the OS cursor is locked, so io.MousePos is GLFW's virtual
        // cursor position and keeps drifting past the panel rect. Bounds-testing it would
        // cancel the drag mid-way, so only the button release below can end it. The test is
        // on our own state, not on Editor::GetMouseCaptured(): the capture is global, and a
        // viewport manipulator holding it must not let clicks through to this canvas.
        const bool draggingCanvas = (state == States::CANVAS_PANING || state == States::CANVAS_ZOOMING);
        if (!popupOpen && (draggingCanvas || widgetBoundingBox.Contains(ImGui::GetMousePos()))) {
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

    // Forget the interaction state tied to the nodes of the previous sheet, called when switching sheets
    void ResetInteraction() {
        if (state == States::CANVAS_PANING || state == States::CANVAS_ZOOMING) {
            Editor::SetMouseCaptured(false);
        }
        state = States::HOVERING_CANVAS;
        nodeClickedPath = SdfPath::EmptyPath();
        connectorTailClicked = SdfPath::EmptyPath();
        connectorHeadClicked = SdfPath::EmptyPath();
        pendingConnectionSource = SdfPath::EmptyPath();
        selectedConnections.clear();
    }

    // Adjust zoom and scroll so all nodes are visible in the current widget.
    // Must be called after Begin() so widgetSize is up to date.
    void FitView(const ConnectionsSheet &sheet, bool selectedOnly = false) {
        if (sheet.nodes.empty() || widgetSize.x <= 0.f || widgetSize.y <= 0.f) return;

        constexpr float fitPadding  = 40.f; // screen-space margin around the graph
        constexpr float headerHeight = 50.f;
        constexpr float lineHeight   = 15.f;

        ImVec2 bboxMin( FLT_MAX,  FLT_MAX);
        ImVec2 bboxMax(-FLT_MAX, -FLT_MAX);
        for (const auto &node : sheet.nodes) {
            if (selectedOnly && !node.selected) continue;
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

    // Zoom by a multiplicative factor while keeping posInScreen anchored under the cursor.
    // Used by the mouse-wheel zoom.
    inline void ZoomAtScreenPosition(const ImVec2 &posInScreen, float factor) {
        const ImVec2 posInCanvas = ScreenToCanvas(posInScreen);
        zooming = std::max(0.01f, std::min(zooming * factor, 50.f));
        // Re-anchor: shift scrolling so posInCanvas maps back to posInScreen.
        scrolling -= CanvasToScreen(posInCanvas) - posInScreen;
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

    // primSelected: the prim of the node is selected in the editor (outliner, viewport, ...), which is different
    // from node.selected, the selection local to the connection editor
    // isRoot: the node is the root prim of a container sheet
    void DrawNode(UsdPrimNode &node, bool primSelected, bool isRoot) {
        if (!node.prim.IsValid()) return;
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
        if (nodeBoundingBox.Contains(ImGui::GetMousePos()))
            mouseOverAnyNode = true;

        // Ring around the root node of a container sheet, outside the selection halo so both stay visible
        if (isRoot) {
            const ImVec2 ringOffset(8.f, 8.f); // in screen space
            drawList->AddRect(CanvasToScreen(nodeMin) - ringOffset, CanvasToScreen(nodeMax) + ringOffset,
                              IM_COL32(80, 200, 255, 220), 8.0f, 0, 2.f);
        }
        // Soft halo around the node when its prim is selected, drawn first so it stays behind the node
        if (primSelected) {
            const ImVec2 haloOffset(3.f, 3.f); // in screen space
            drawList->AddRect(CanvasToScreen(nodeMin) - haloOffset, CanvasToScreen(nodeMax) + haloOffset,
                              IM_COL32(255, 190, 50, 140), 6.0f, 0, 4.f);
        }
        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), 0xFF090920, 4.0f);
        const ImVec2 headerMax = ImVec2(nodeMax.x, nodeMin.y + headerHeight);
        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(headerMax),
                                GetNodeHeaderColor(node.prim), 4.0f, ImDrawFlags_RoundCornersTop);
        drawList->AddRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), node.selected ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 255, 255), 4.0f);

        // TODO: add padding, truncate name if too long, add tooltip
        const float scaledFontSize = g.FontSize * zooming;
        if (scaledFontSize >= 1.f) {
            const ImVec2 namePos = CanvasToScreen(nodeMin) + ImVec2(4.f, 0.f);
            drawList->AddText(g.Font, scaledFontSize, namePos, IM_COL32(255, 255, 255, 255), node.prim.GetName().GetText());
            {
                static const TfToken shaderToken("Shader");
                static const TfToken infoIdToken("info:id");
                if (node.prim.GetTypeName() == shaderToken) {
                    UsdAttribute infoIdAttr = node.prim.GetAttribute(infoIdToken);
                    if (infoIdAttr) {
                        TfToken shaderType;
                        if (infoIdAttr.Get(&shaderType) && !shaderType.IsEmpty()) {
                            const ImVec2 subtitlePos = CanvasToScreen(nodeMin) + ImVec2(4.f, scaledFontSize);
                            drawList->AddText(g.Font, scaledFontSize * 0.85f, subtitlePos,
                                              IM_COL32(200, 200, 200, 200), shaderType.GetText());
                        }
                    }
                }
            }
        }
        
        // Check if the user clicked on the node and update the event.
        // The event might be again updated later on, on the connectors as well, that's how choosing the event is implemented
        if (ImGui::IsMouseClicked(0)) {
            if (nodeBoundingBox.Contains(ImGui::GetMousePos())) {
                event = Events::NODE_CLICKED; // Node clicked
                nodeClickedPath = node.primPath;
                nodeClickedSelected = node.selected;
            }
        }
        // Double-click on a node, consumed in DrawConnectionEditor to open the container sheets
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nodeBoundingBox.Contains(ImGui::GetMousePos())) {
            nodeDoubleClickedPath = node.primPath;
        }
        // Right-click on a node opens its context menu (alt+right is reserved for zoom)
        if (ImGui::IsMouseClicked(1) && !ImGui::IsKeyDown(ImGuiKey_LeftAlt)
            && nodeBoundingBox.Contains(ImGui::GetMousePos())) {
            openNodeContextMenu = true;
            contextNodePath = node.primPath;
            contextNodeSelected = node.selected;
        }
        // "Select prim" button — top-right corner of the header, same size as connectors.
        // Overrides NODE_CLICKED when the button is the actual click target.
        {
            const ImVec2 btnMin(nodeMax.x - 2*connectorSize - 2.f, nodeMin.y + 2.f);
            const ImVec2 btnMax(nodeMax.x - 2.f, nodeMin.y + 2*connectorSize + 2.f);
            ImRect btnBB(CanvasToScreen(btnMin), CanvasToScreen(btnMax));
            btnBB.ClipWith(widgetBoundingBox);
            const bool btnHovered = btnBB.Contains(ImGui::GetMousePos());
            const ImU32 btnColor = btnHovered     ? IM_COL32(255, 220, 120, 255)
                                   : primSelected ? IM_COL32(255, 190, 50, 230)
                                                  : IM_COL32(180, 140, 40, 160);
            drawList->AddRectFilled(CanvasToScreen(btnMin), CanvasToScreen(btnMax), btnColor, 2.f);
            //drawList->AddText(g.Font, g.FontSize * zooming * 0.85f, CanvasToScreen(btnMin), IM_COL32(255, 255, 255, 255), "S");
            if (ImGui::IsMouseClicked(0) && btnBB.Contains(ImGui::GetMousePos())) {
                event = Events::SELECT_PRIM_CLICKED;
                nodeClickedPath = node.primPath;
                nodeClickedSelected = node.selected;
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
            if (state == CONNECTING_NODES || !pendingConnectionSource.IsEmpty()) {
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
            if (scaledFontSize >= 1.f)
                drawList->AddText(g.Font, scaledFontSize, CanvasToScreen(textPos),
                                  node.authoredProperties[i] ? IM_COL32(255, 190, 190, 255) : IM_COL32(255, 255, 255, 255),
                                  properties[i].GetNameToken().GetText());
            
            // Update positions of the connectors
            connectorPositions[properties[i]] = ImVec4(nodeMin.x, inputStartPos.y + 9.f + linePos, nodeMax.x, inputStartPos.y + 9.f + linePos);
        }

        // "+" row after the properties, it opens a menu to connect the properties which are not displayed.
        // The node height already has room for it.
        if (showConnectRow) {
            const float linePos = properties.size() * (g.FontSize + 2.f);
            const ImVec2 rowMin(nodeMin.x, inputStartPos.y + linePos);
            const ImVec2 rowMax(nodeMax.x, inputStartPos.y + linePos + g.FontSize + 2.f);
            ImRect rowBoundingBox(CanvasToScreen(rowMin), CanvasToScreen(rowMax));
            rowBoundingBox.ClipWith(widgetBoundingBox);
            const bool rowHovered = rowBoundingBox.Contains(ImGui::GetMousePos());
            if (rowHovered) {
                drawList->AddRectFilled(CanvasToScreen(rowMin), CanvasToScreen(rowMax), IM_COL32(255, 255, 255, 40));
            }
            // The "+" is orange when it can receive the pending connection
            const ImU32 textColor = pendingConnectionSource.IsEmpty() ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 127, 17, 255);
            if (scaledFontSize >= 1.f)
                drawList->AddText(g.Font, scaledFontSize, CanvasToScreen(inputStartPos + ImVec2(0, linePos)), textColor, "+");
            if (ImGui::IsMouseClicked(0) && rowHovered) {
                event = Events::CONNECT_ROW_CLICKED;
                attributeMenuNodePath = node.primPath;
            }
            // Stored with the prim path, it's the start of the pending connection when its source isn't displayed
            connectorPositions[node.primPath] = ImVec4(nodeMin.x, inputStartPos.y + 9.f + linePos, nodeMax.x, inputStartPos.y + 9.f + linePos);
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
        } else if (state == MOVING_NODE && !nodeClickedPath.IsEmpty()) {
            // Dragging a selected node moves the whole selection, otherwise only the clicked node
            if (nodeClickedSelected ? node.selected : node.primPath == nodeClickedPath) {
                // TODO this could be a function UpdateNodePosition(node)
                node.position = node.position + io.MouseDelta/zooming;
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
    
    void DrawSheet(ConnectionsSheet &sheet, const UsdStageRefPtr &stage, const Selection &selection) {
        ImGuiContext& g = *GImGui;
        ImGuiIO& io = ImGui::GetIO();
        currentStage = stage;
        connectorPositions.clear();
        connectorHeadClicked = SdfPath::EmptyPath(); // set again by DrawNode if the mouse is over an input connector
        //ImGuiWindow* window = g.CurrentWindow;
        // Give the node a position in canvas coordinates
        drawList->ChannelsSetCurrent(1); // Foreground
        mouseOverAnyNode = false;
        for (auto &node : sheet.nodes) {
            DrawNode(node, selection.IsSelected(stage, node.primPath), sheet.IsRootNode(node));
        }

        // A right-click on a node that isn't part of the current selection makes it the
        // sole selection, so the context menu acts on a visible, unambiguous target.
        // (Right-clicking an already-selected node keeps the whole node selection.)
        if (openNodeContextMenu && !contextNodeSelected) {
            for (auto &node : sheet.nodes)
                node.selected = (node.primPath == contextNodePath);
            contextNodeSelected = true;
            hasSelectedNodes = true;
        }

        drawList->ChannelsSetCurrent(0); // Background

        // Reset hovered edge each frame
        hoveredConnectionHead = SdfPath::EmptyPath();
        hoveredConnectionTail = SdfPath::EmptyPath();

        auto isEdgeSelected = [&](const NodeConnection& c) {
            return std::any_of(selectedConnections.begin(), selectedConnections.end(),
                [&](const NodeConnection& s) { return s.begin == c.begin && s.end == c.end; });
        };

        const ImVec2 mouse = ImGui::GetMousePos();
        const float tessellationTol = ImGui::GetStyle().CurveTessellationTol;
        const float hitRadiusSq = 8.f * 8.f;

        for (const auto &con : sheet.connections) {
            const auto arrowHead = connectorPositions.find(con.begin);
            if (arrowHead == connectorPositions.end()) continue;
            const auto arrowTail = connectorPositions.find(con.end);
            if (arrowTail == connectorPositions.end()) continue;

            const float dx = arrowHead->second.x - arrowTail->second.z;
            const float dy = arrowHead->second.y - arrowTail->second.w;
            float tangent;
            ImU32 normalColor;
            if (dx >= 0.f) {
                tangent = std::min(150.f, dx * 0.5f);
                normalColor = IM_COL32(255, 255, 255, 255);
            } else {
                tangent = std::max(100.f, (std::abs(dx) + std::abs(dy)) * 0.35f);
                normalColor = IM_COL32(190, 190, 190, 180);
            }

            ImVec2 p1a(arrowTail->second.z, arrowTail->second.w);
            ImVec2 p1b(arrowTail->second.z + tangent, arrowTail->second.w);
            ImVec2 p2b(arrowHead->second.x - tangent, arrowHead->second.y);
            ImVec2 p2a(arrowHead->second.x, arrowHead->second.y);

            // Hit-test against the Bezier curve in screen space
            ImVec2 sp1a = CanvasToScreen(p1a);
            ImVec2 sp1b = CanvasToScreen(p1b);
            ImVec2 sp2b = CanvasToScreen(p2b);
            ImVec2 sp2a = CanvasToScreen(p2a);

            const ImVec2 closest = ImBezierCubicClosestPointCasteljau(sp1a, sp1b, sp2b, sp2a, mouse, tessellationTol);
            const float cdx = closest.x - mouse.x;
            const float cdy = closest.y - mouse.y;
            const bool hovered = !mouseOverAnyNode && (cdx * cdx + cdy * cdy) < hitRadiusSq;

            if (hovered) {
                hoveredConnectionHead = con.begin;
                hoveredConnectionTail = con.end;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_LeftAlt) && event != Events::CONNECTOR_CLICKED) {
                    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
                        // Toggle this edge in/out of selection
                        auto it = std::find_if(selectedConnections.begin(), selectedConnections.end(),
                            [&](const NodeConnection& s) { return s.begin == con.begin && s.end == con.end; });
                        if (it != selectedConnections.end())
                            selectedConnections.erase(it);
                        else
                            selectedConnections.push_back(con);
                    } else {
                        selectedConnections.clear();
                        selectedConnections.push_back(con);
                    }
                    event = Events::CONNECTION_CLICKED;
                }
            }

            const bool selected = isEdgeSelected(con);
            ImU32 color = selected ? IM_COL32(255, 165,   0, 255)
                        : hovered  ? IM_COL32(255, 220, 100, 255)
                        :            normalColor;
            float thickness = selected ? 3.f : 2.f;
            drawList->AddBezierCubic(sp1a, sp1b, sp2b, sp2a, color, thickness);
        }
        
        // Show connecting node
        if (state == CONNECTING_NODES) {
            // The source node might have been removed while connecting
            const auto arrowTail = connectorPositions.find(connectorTailClicked);
            if (arrowTail != connectorPositions.end()) {
                ImVec2 p1(arrowTail->second.z, arrowTail->second.w);
                drawList->AddLine(CanvasToScreen(p1), ImGui::GetMousePos(), IM_COL32(255, 127, 127, 255));
            }
        }

        // Show the connection started from a "+" row, it starts from the source property if it's displayed,
        // otherwise from the "+" row of its node
        if (!pendingConnectionSource.IsEmpty()) {
            auto sourcePosition = connectorPositions.find(pendingConnectionSource);
            if (sourcePosition == connectorPositions.end()) {
                sourcePosition = connectorPositions.find(pendingConnectionSource.GetPrimPath());
            }
            if (sourcePosition != connectorPositions.end()) {
                ImVec2 p1(sourcePosition->second.z, sourcePosition->second.w);
                drawList->AddLine(CanvasToScreen(p1), ImGui::GetMousePos(), IM_COL32(255, 127, 17, 255), 2.f);
            } else {
                pendingConnectionSource = SdfPath::EmptyPath(); // the source node is not in the sheet anymore
            }
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
        // A connection started from a "+" row: clicking on an input connector completes it, escape cancels it
        if (!pendingConnectionSource.IsEmpty() && state == States::HOVERING_CANVAS) {
            if ((event == NODE_CLICKED || event == CANVAS_CLICKED) && !connectorHeadClicked.IsEmpty()) {
                if (connectorHeadClicked != pendingConnectionSource) { // an attribute can't be connected to itself
                    ConnectAttributes(currentStage, pendingConnectionSource, pendingConnectionSourceType,
                                      connectorHeadClicked, SdfValueTypeName());
                    pendingConnectionSource = SdfPath::EmptyPath();
                }
                event = IDLE; // the click is consumed
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                pendingConnectionSource = SdfPath::EmptyPath();
            }
        }
        if (state ==States::HOVERING_CANVAS) {
            if (event == CONNECT_ROW_CLICKED) {
                openAttributeMenu = !readOnly; // the menu is drawn by DrawConnectionEditor
            } else if (event == CONNECTION_CLICKED) {
                // Selection already set in DrawSheet; just stay in HOVERING_CANVAS
            } else if (event == SELECT_PRIM_CLICKED) {
                if (!nodeClickedPath.IsEmpty()) {
                    ExecuteAfterDraw<EditorSetSelection>(currentStage, nodeClickedPath);
                }
            } else if (event == NODE_CLICKED) {
                selectedConnections.clear();
                state = SELECTING_NODE; // Single node selection,
            } else if (event == CANVAS_CLICKED) {
                pendingConnectionSource = SdfPath::EmptyPath(); // clicking on the canvas cancels the connection
                selectedConnections.clear();
                state = SELECTING_REGION; // Region selection
                selectionOrigin = ImGui::GetMousePos();
            } else if (event == CANVAS_CLICKED_PANNING) {
                state = CANVAS_PANING;
                Editor::SetMouseCaptured(true);
            } else if (event == CANVAS_CLICKED_ZOOMING) {
                state = CANVAS_ZOOMING;
                Editor::SetMouseCaptured(true);
            } else if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
            } else if (event == CONNECTOR_CLICKED) {
                if (readOnly) {
                    connectorTailClicked = SdfPath::EmptyPath();
                } else {
                    state = CONNECTING_NODES;
                }
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
                Editor::SetMouseCaptured(false);
            }
            // Update scrolling
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
                scrolling = scrolling + io.MouseDelta;
            }
        } else if (state == CANVAS_ZOOMING) {
            if (event == CLICK_RELEASED || !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                state = HOVERING_CANVAS;
                Editor::SetMouseCaptured(false);
            }
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
                ZoomFromPosition(zoomClick, io.MouseDelta);
            }
        } else if (state == MOVING_NODE) {
            if (event == CLICK_RELEASED) {
                state = HOVERING_CANVAS;
                nodeClickedPath = SdfPath::EmptyPath();
            }
        } else if (state == CONNECTING_NODES) {
            if (event == CLICK_RELEASED) {
                if (connectorHeadClicked != SdfPath::EmptyPath() && connectorHeadClicked != connectorTailClicked) {
                    // Launch a command !
                    ExecuteAfterDraw<AttributeConnect>(currentStage, connectorTailClicked, connectorHeadClicked);
                }
                state = HOVERING_CANVAS;
                connectorTailClicked = SdfPath::EmptyPath();
                connectorHeadClicked = SdfPath::EmptyPath();
            }
        }
        // State debugging
        //ImGuiContext& g = *GImGui;
        //if (g.FontSize * zooming >= 4.f) {
        //    std::string stateDebug = "State " + std::to_string(state);
        //    drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(ImVec2(0.2, 0.2)), IM_COL32(255, 255, 255, 255), stateDebug.c_str());
        //}
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
    // Node clicked, stored by path as pointers into the sheet nodes are invalidated when nodes are added or removed
    SdfPath nodeClickedPath;
    bool nodeClickedSelected = false; // selection state of the clicked node at click time
    SdfPath nodeDoubleClickedPath; // reset each frame

    // The connections can't be edited, set when drawing the sheet of a prim inside an instance or prototype
    bool readOnly = false;

    // Right-click node context menu (opened in DrawNode, consumed in DrawConnectionEditor)
    bool openNodeContextMenu = false;
    SdfPath contextNodePath;
    bool contextNodeSelected = false;

    SdfPath connectorTailClicked;
    SdfPath connectorHeadClicked;

    // "+" row: shown when only the connected properties are displayed. The source chosen in the menu of a first
    // node is kept in pendingConnectionSource until the destination is chosen in the menu of a second node.
    bool showConnectRow = false;
    bool openAttributeMenu = false;
    SdfPath attributeMenuNodePath;
    SdfPath pendingConnectionSource;
    SdfValueTypeName pendingConnectionSourceType; // set when the source doesn't exist yet and must be created

    // Connection (edge) multi-selection — stored as SdfPath pairs, stable across Update() rebuilds
    std::vector<NodeConnection> selectedConnections;
    SdfPath hoveredConnectionHead;
    SdfPath hoveredConnectionTail;

    // Connectors positions
    std::unordered_map<SdfPath, ImVec4, SdfPath::Hash> connectorPositions; // 2 in 2 out
    // std::unordered_map<SdfPath, ImVec2, SdfPath::Hash> outputsPositions;

    bool hasSelectedNodes = false; // computed at each frame
    bool mouseOverAnyNode = false; // reset each frame; true when mouse is inside any node bounding box
    
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

// Delete the prims of a container sheet in a single undoable command. A prim defined in the edit target layer is
// removed from it, the others (defined in a weaker layer or a reference) or the ones still composed after the removal
// are deactivated. A prim only overridden in the edit target keeps its overrides.
// The prims are siblings, the children of the root prim. The connections to them from the root prim and its other
// descendants are removed as well, a dangling connection would not be visible in the sheet.
static void DeletePrims(const UsdStageWeakPtr &stage, std::vector<SdfPath> paths) {
    if (!stage || paths.empty())
        return;
    std::function<void()> deletePrims = [=]() {
        if (!stage)
            return;
        const std::unordered_set<SdfPath, SdfPath::Hash> deletedPaths(paths.begin(), paths.end());
        const auto isDeleted = [&](const SdfPath &target) {
            const SdfPath primPath = target.GetPrimPath();
            return std::any_of(deletedPaths.begin(), deletedPaths.end(),
                               [&](const SdfPath &path) { return primPath.HasPrefix(path); });
        };
        if (UsdPrim parent = stage->GetPrimAtPath(paths.front().GetParentPath())) {
            UsdPrimRange range(parent, UsdPrimAllPrimsPredicate); // the inactive prims can be reactivated
            for (auto primIt = range.begin(); primIt != range.end(); ++primIt) {
                if (deletedPaths.count(primIt->GetPath())) {
                    primIt.PruneChildren();
                    continue;
                }
                for (const UsdAttribute &attribute : primIt->GetAttributes()) {
                    SdfPathVector targets;
                    attribute.GetConnections(&targets);
                    for (const SdfPath &target : targets) {
                        if (isDeleted(target)) {
                            attribute.RemoveConnection(target);
                        }
                    }
                }
            }
        }
        const UsdEditTarget &editTarget = stage->GetEditTarget();
        for (const SdfPath &path : paths) {
            const SdfPrimSpecHandle spec = editTarget.GetPrimSpecForScenePath(path);
            if (spec && spec->GetSpecifier() == SdfSpecifierDef) {
                stage->RemovePrim(path);
            }
            if (UsdPrim prim = stage->GetPrimAtPath(path)) {
                prim.SetActive(false);
            }
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(TfCreateRefPtrFromProtectedWeakPtr(stage->GetEditTarget().GetLayer()), deletePrims);
}

// Prim created from the Tab menu of a container sheet: a Material, a NodeGraph, or a Shader when shaderId is set
struct NodeToCreate {
    TfToken typeName;
    TfToken shaderId;
};

// Create the prim under parentPath, with a unique name and its node position, in a single undoable command
static void CreateNodePrim(const UsdStageWeakPtr &stage, const SdfPath &parentPath, const NodeToCreate &node,
                           const ImVec2 &position) {
    if (!stage)
        return;
    std::function<void()> create = [=]() {
        if (!stage)
            return;
        // Unique sibling name, derived from the shader id
        const std::string baseName =
            TfMakeValidIdentifier(node.shaderId.IsEmpty() ? node.typeName.GetString() : node.shaderId.GetString());
        SdfPath path = parentPath.AppendChild(TfToken(baseName));
        for (int index = 1; stage->GetPrimAtPath(path); index++) {
            path = parentPath.AppendChild(TfToken(baseName + std::to_string(index)));
        }
        UsdPrim prim;
        if (node.typeName == UsdShadeTokens->Material) {
            prim = UsdShadeMaterial::Define(stage, path).GetPrim();
        } else if (node.typeName == UsdShadeTokens->NodeGraph) {
            prim = UsdShadeNodeGraph::Define(stage, path).GetPrim();
        } else {
            UsdShadeShader shader = UsdShadeShader::Define(stage, path);
            shader.CreateIdAttr(VtValue(node.shaderId));
            prim = shader.GetPrim();
        }
        if (prim) {
            UsdUINodeGraphNodeAPI::Apply(prim).CreatePosAttr(VtValue(GfVec2f(position.x, position.y)));
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(TfCreateRefPtrFromProtectedWeakPtr(stage->GetEditTarget().GetLayer()), create);
}

// Identifier of a shader with its lower case version used by the Tab menu filter
struct ShaderIdentifier {
    TfToken id;
    std::string lowerId;
};

// Identifiers of the shaders known by the Sdr registry, sorted. They are read once, without parsing the shaders.
static const std::vector<ShaderIdentifier> &GetShaderIdentifiers() {
    static std::vector<ShaderIdentifier> identifiers;
    static bool initialized = false;
    if (!initialized) {
#if PXR_VERSION >= 2505
        auto ids = SdrRegistry::GetInstance().GetShaderNodeIdentifiers();
#else
        auto ids = SdrRegistry::GetInstance().GetNodeIdentifiers();
#endif
        std::sort(ids.begin(), ids.end(), [](const TfToken &a, const TfToken &b) { return a.GetString() < b.GetString(); });
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        identifiers.reserve(ids.size());
        for (const TfToken &id : ids) {
            identifiers.push_back({id, TfStringToLower(id.GetString())});
        }
        initialized = true;
    }
    return identifiers;
}

// Tab menu of the container sheets, it creates a prim under the root prim at the given canvas position.
// Enter creates the first entry matching the filter.
static void DrawCreateNodeMenu(const UsdStageWeakPtr &stage, const SdfPath &rootPath, const ImVec2 &position) {
    static char filter[128] = {};
    if (ImGui::IsWindowAppearing()) {
        filter[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    const bool enterPressed = ImGui::InputTextWithHint("##CreateNodeFilter", "Filter", filter, sizeof(filter),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string lowerFilter = TfStringToLower(filter);
    auto passesFilter = [&](const std::string &lowerName) {
        return lowerFilter.empty() || lowerName.find(lowerFilter) != std::string::npos;
    };

    std::vector<NodeToCreate> entries;
    for (const TfToken &typeName : {UsdShadeTokens->Material, UsdShadeTokens->NodeGraph}) {
        if (passesFilter(TfStringToLower(typeName.GetString())))
            entries.push_back({typeName, TfToken()});
    }
    for (const ShaderIdentifier &shader : GetShaderIdentifiers()) {
        if (passesFilter(shader.lowerId))
            entries.push_back({UsdShadeTokens->Shader, shader.id});
    }

    const NodeToCreate *chosen = enterPressed && !entries.empty() ? &entries.front() : nullptr;
    ImGui::BeginChild("##CreateNodeEntries", ImVec2(300.f, std::min(300.f, (entries.size() + 1) * ImGui::GetFrameHeight())));
    for (const NodeToCreate &entry : entries) {
        const bool isShader = !entry.shaderId.IsEmpty();
        if (ImGui::MenuItem(isShader ? entry.shaderId.GetText() : entry.typeName.GetText(), isShader ? nullptr : "prim")) {
            chosen = &entry;
        }
    }
    ImGui::EndChild();
    if (chosen) {
        CreateNodePrim(stage, rootPath, *chosen, position);
        ImGui::CloseCurrentPopup();
    }
}

// Bake the node positions of the sheet in ui:nodegraph:node:pos
static void BakeNodePositions(const UsdStageRefPtr &stage, const ConnectionsSheet &sheet) {
    std::vector<std::pair<SdfPath, ImVec2>> posData;
    posData.reserve(sheet.nodes.size());
    for (const auto &node : sheet.nodes)
        posData.emplace_back(node.primPath, node.position);
    ExecuteAfterDraw([posData](UsdStageRefPtr stage) {
        for (const auto &[path, pos] : posData) {
            UsdPrim prim = stage->GetPrimAtPath(path);
            if (prim) {
                UsdUINodeGraphNodeAPI::Apply(prim)
                    .CreatePosAttr(VtValue(), false)
                    .Set(GfVec2f(pos.x, pos.y));
            }
        }
    }, stage);
}

// The menus act on the selected sheet, and the display options are stored per sheet
static void DrawConnectionEditorMenuBar(const UsdStageRefPtr &stage, const Selection &selection, ConnectionsSheet &sheet,
                                        bool &fitView) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Show")) {
            ImGui::MenuItem("Show all parameters", nullptr, &sheet.showAllParameters);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Actions")) {
            // The content of a container sheet follows its root prim, prims can't be imported in it
            if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import Selected", nullptr, false, !sheet.isContainer)) {
                std::vector<UsdPrim> seeds;
                for (const SdfPath &path : selection.GetSelectedPaths(stage)) {
                    if (UsdPrim prim = stage->GetPrimAtPath(path))
                        seeds.push_back(prim);
                }
                AddConnectedPrimsToCurrentSession(seeds);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(sheet.isContainer ? "Only available in the default sheet"
                                                    : "Import the selected prims and everything connected to them");
            }
            if (ImGui::MenuItem(ICON_FA_PROJECT_DIAGRAM " Auto Layout")) {
                AutoLayout(sheet);
                fitView = true;
            }
            // The positions can't be authored on instance proxies
            if (ImGui::MenuItem(ICON_FA_SAVE " Bake Positions", nullptr, false, !sheet.IsReadOnly())) {
                BakeNodePositions(stage, sheet);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void DrawConnectionEditor(const UsdStageRefPtr &stage, const Selection &selection) {
    // We are maintaining a list of graph edit session per stage
    // Each session contains a list of edited node, node visible on the whiteboard
    // Initialization
    static ConnectionsEditorCanvas canvas;
    StageSheets *sheets = editorData.GetSheets(stage);
    if (sheets) {
        sheets->DeleteExpiredContainerSheets(stage);
        // Sheet tabs. The selected sheet is owned by StageSheets: its tab is forced selected every frame, and a click
        // on another tab changes it, so the sheets can also be selected from outside the editor (outliner).
        // One tab bar per stage, so each one keeps its own state.
        StageSheets::SheetID sheetToDelete;
        ImGui::PushID(sheets);
        if (ImGui::BeginTabBar("##StageSheets")) {
            for (const auto &sheetID : sheets->GetSheetNames()) {
                const bool isDefault = sheets->IsDefaultSheet(sheetID);
                // The label is the root prim name, the ID after ### is the unique sheet ID
                const std::string label = (isDefault ? sheetID : SdfPath(sheetID).GetName()) + "###" + sheetID;
                const ImGuiTabItemFlags flags =
                    sheetID == sheets->GetSelectedSheetID() ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                bool open = true;
                if (ImGui::BeginTabItem(label.c_str(), isDefault ? nullptr : &open, flags)) {
                    ImGui::EndTabItem();
                }
                // The close button is part of the tab item, closing a tab must not select it
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && GImGui->HoveredId == ImGui::GetItemID()) {
                    sheets->SetSelectedSheet(sheetID);
                }
                if (!isDefault && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", sheetID.c_str());
                }
                if (!open) {
                    sheetToDelete = sheetID; // deleted after the loop which iterates on the sheet names
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::PopID();
        if (!sheetToDelete.empty()) {
            sheets->DeleteSheet(sheetToDelete);
        }
        ConnectionsSheet &sheet = sheets->GetSelectedSheet();

        // The interaction state refers to the nodes of the sheet it started on, it's reset when the drawn sheet
        // changes, either by switching tab or stage
        // A pending fit view was requested for the previous sheet, it must not change the view of the new one.
        static const StageSheets *lastSheets = nullptr;
        static StageSheets::SheetID lastSheetID;
        static bool pendingFitView = false;
        static bool pendingFitViewSelected = false;
        if (lastSheets != sheets || lastSheetID != sheets->GetSelectedSheetID()) {
            canvas.ResetInteraction();
            pendingFitView = false;
            pendingFitViewSelected = false;
            lastSheets = sheets;
            lastSheetID = sheets->GetSelectedSheetID();
        }
        canvas.scrolling = sheet.scrolling;
        canvas.zooming = sheet.zooming;

        // Update the node positions, inputs, outputs`
        // update the connections as well
        sheet.Update(stage);

        // A new container sheet is framed, and laid out when none of its prims has a position in usd
        if (sheet.needsInitialLayout) {
            if (std::none_of(sheet.nodes.begin(), sheet.nodes.end(),
                             [](const UsdPrimNode &node) { return node.hasUsdPosition; })) {
                AutoLayout(sheet);
            }
            pendingFitView = true;
            sheet.needsInitialLayout = false;
        }

        // Drawn after the update of the sheet as its actions use the nodes
        DrawConnectionEditorMenuBar(stage, selection, sheet, pendingFitView);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        canvas.Begin(drawList);
        if (pendingFitView) {
            canvas.FitView(sheet, pendingFitViewSelected);
            pendingFitView = false;
            pendingFitViewSelected = false;
        }
        // The window is not hovered when the mouse is over one of its popups (Tab menu, context menus), which are drawn
        // over the canvas, and the shortcuts are ignored while a text field (the Tab menu filter) is edited
        const bool canvasHovered = ImGui::IsWindowHovered() && canvas.widgetBoundingBox.Contains(ImGui::GetMousePos());
        const bool canvasShortcuts = canvasHovered && !ImGui::GetIO().WantTextInput;
        // Mouse-wheel zoom, anchored under the cursor.
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f && canvasHovered) {
            canvas.ZoomAtScreenPosition(ImGui::GetMousePos(), powf(1.1f, wheel));
        }
        canvas.DrawGrid();
        canvas.readOnly = sheet.IsReadOnly();
        canvas.showConnectRow = !sheet.showAllParameters;
        if (!canvas.showConnectRow) {
            canvas.pendingConnectionSource = SdfPath::EmptyPath();
        }
        canvas.DrawSheet(sheet, stage, selection);
        //canvas.DrawBoundaries(); // for debugging
        //canvas.DrawNodeTest(); // for debugging
        canvas.DrawRegionSelection();
        canvas.UpdateState(); // Might move in End ???
        // canvas.ProcessActions() // TODO ??

        // Double-click on a container node opens its sheet
        if (canvasHovered && !canvas.nodeDoubleClickedPath.IsEmpty()
            && !(sheet.isContainer && canvas.nodeDoubleClickedPath == sheet.rootPath)) {
            const UsdPrim prim = stage->GetPrimAtPath(canvas.nodeDoubleClickedPath);
            if (IsContainerPrim(prim)) {
                OpenContainerSheet(prim);
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F) && canvasShortcuts) {
            pendingFitView = true;
            pendingFitViewSelected = canvas.hasSelectedNodes;
        }

        // Tab opens the node creation menu of the container sheets, the node is created under the mouse cursor.
        // Shortcut takes the key ownership, otherwise imgui also uses Tab to move the focus between the widgets.
        static ImVec2 createNodePosition;
        const bool openCreateNodeMenu = sheet.isContainer && !sheet.IsReadOnly() && ImGui::Shortcut(ImGuiKey_Tab)
                                        && canvasShortcuts && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (openCreateNodeMenu) {
            createNodePosition = canvas.ScreenToCanvas(ImGui::GetMousePos());
        }

        // In a container sheet the prims of the selected nodes are deleted, except the root prim which is protected.
        // In the other sheets the nodes are only removed from the sheet.
        const bool canRemoveSelectedNodes =
            sheet.isContainer ? !sheet.IsReadOnly() && std::any_of(sheet.nodes.begin(), sheet.nodes.end(),
                                                                    [&](const UsdPrimNode &node) {
                                                                        return node.selected && !sheet.IsRootNode(node);
                                                                    })
                              : canvas.hasSelectedNodes;
        auto removeSelectedNodes = [&]() {
            if (!canRemoveSelectedNodes)
                return;
            if (sheet.isContainer) {
                std::vector<SdfPath> paths;
                for (const UsdPrimNode &node : sheet.nodes) {
                    if (node.selected && !sheet.IsRootNode(node))
                        paths.push_back(node.primPath);
                }
                DeletePrims(stage, std::move(paths));
            } else {
                sheet.nodes.erase(std::remove_if(sheet.nodes.begin(), sheet.nodes.end(),
                                                 [](const UsdPrimNode &node) { return node.selected; }),
                                  sheet.nodes.end());
            }
        };

        // Backspace deletes the selected connections (a real stage edit) if any are
        // selected, otherwise it removes the selected nodes.
        const bool backspacePressed = ImGui::IsKeyPressed(ImGuiKey_Backspace) && canvasShortcuts;
        if (backspacePressed && !canvas.selectedConnections.empty()) {
            if (!canvas.readOnly) {
                std::vector<std::pair<SdfPath, SdfPath>> toDelete;
                toDelete.reserve(canvas.selectedConnections.size());
                for (const auto &c : canvas.selectedConnections)
                    toDelete.emplace_back(c.begin, c.end);
                ExecuteAfterDraw<AttributeDisconnectBatch>(canvas.currentStage, std::move(toDelete));
                canvas.selectedConnections.clear();
            }
        } else if (backspacePressed) {
            removeSelectedNodes();
        }

        canvas.End();
        sheet.scrolling = canvas.scrolling;
        sheet.zooming = canvas.zooming;

        // Node context menu (right-click). A right-click on an unselected node acts on
        // that node alone; on a selected node it acts on the whole node selection.
        if (canvas.openNodeContextMenu)
            ImGui::OpenPopup("##NodeContextMenu");
        if (ImGui::BeginPopup("##NodeContextMenu")) {
            if (ImGui::MenuItem(ICON_FA_MOUSE_POINTER " Select prim")) {
                if (stage->GetPrimAtPath(canvas.contextNodePath))
                    ExecuteAfterDraw<EditorSetSelection>(stage, canvas.contextNodePath);
            }
            const UsdPrim contextPrim = stage->GetPrimAtPath(canvas.contextNodePath);
            if (IsContainerPrim(contextPrim) && !(sheet.isContainer && canvas.contextNodePath == sheet.rootPath)) {
                if (ImGui::MenuItem(ICON_FA_PROJECT_DIAGRAM " Open in connection editor")) {
                    OpenContainerSheet(contextPrim);
                }
            }
            if (ImGui::MenuItem(sheet.isContainer ? ICON_FA_TRASH " Delete prim" : ICON_FA_TRASH " Remove from editor",
                                nullptr, false, canRemoveSelectedNodes)) {
                removeSelectedNodes();
            }
            ImGui::EndPopup();
        }

        if (openCreateNodeMenu)
            ImGui::OpenPopup("##CreateNodeMenu");
        if (ImGui::BeginPopup("##CreateNodeMenu")) {
            DrawCreateNodeMenu(stage, sheet.rootPath, createNodePosition);
            ImGui::EndPopup();
        }

        // "+" row menu. Without a pending connection it chooses the source attribute, otherwise the destination
        // attribute, which is then connected to the source.
        if (canvas.openAttributeMenu)
            ImGui::OpenPopup("##AttributeMenu");
        if (ImGui::BeginPopup("##AttributeMenu")) {
            const bool choosingSource = canvas.pendingConnectionSource.IsEmpty();
            if (choosingSource) {
                ImGui::TextDisabled("Connect from");
            } else {
                ImGui::TextDisabled("Connect %s to", canvas.pendingConnectionSource.GetText());
            }
            ImGui::Separator();
            if (UsdPrim prim = stage->GetPrimAtPath(canvas.attributeMenuNodePath)) {
                bool declaredSectionStarted = false;
                for (const ConnectableAttribute &attribute :
                     GetConnectableAttributes(prim, sheet.showAllParameters)) {
                    const SdfPath attributePath = prim.GetPath().AppendProperty(attribute.name);
                    if (attributePath == canvas.pendingConnectionSource)
                        continue;
                    // The attributes which don't exist yet come last, in their own section
                    if (attribute.typeToCreate && !declaredSectionStarted) {
                        ImGui::Separator();
                        ImGui::TextDisabled("Not authored");
                        declaredSectionStarted = true;
                    }
                    if (ImGui::MenuItem(attribute.name.GetText(),
                                        attribute.typeToCreate ? attribute.typeToCreate.GetAsToken().GetText() : nullptr)) {
                        if (choosingSource) {
                            canvas.pendingConnectionSource = attributePath;
                            canvas.pendingConnectionSourceType = attribute.typeToCreate;
                        } else {
                            ConnectAttributes(canvas.currentStage, canvas.pendingConnectionSource,
                                              canvas.pendingConnectionSourceType, attributePath, attribute.typeToCreate);
                            canvas.pendingConnectionSource = SdfPath::EmptyPath();
                        }
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
}

// Add node to blackboard
// New blackboard
// struct ConnectionEditorBlackboard {
//  std::vector<Node> _nodes
//};


bool CanOpenContainerSheet(const UsdPrim &prim) { return IsContainerPrim(prim); }

void OpenContainerSheet(const UsdPrim &prim) {
    if (!IsContainerPrim(prim))
        return;
    if (StageSheets *sbb = editorData.GetSheets(prim.GetStage())) {
        sbb->SetSelectedSheet(sbb->CreateContainerSheet(prim));
        editorData.focusRequested = true;
    }
}

bool ConsumeConnectionEditorFocusRequest() {
    const bool focusRequested = editorData.focusRequested;
    editorData.focusRequested = false;
    return focusRequested;
}

void AddPrimsToDefaultSheet(const std::vector<UsdPrim> &prims) {
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
            sbb->GetSheet(defaultSheetID).AddNodes(all);
            sbb->SetSelectedSheet(defaultSheetID);
        }
    }
}


void AddConnectedPrimsToCurrentSession(const std::vector<UsdPrim> &seeds) {
    if (seeds.empty()) return;
    // Assuming all the seeds are coming from the same stage.
    UsdStageWeakPtr stage = seeds[0].GetStage();
    if (!stage) return;

    // Build an undirected, prim-level adjacency from every authored attribute
    // connection on the stage. A connection links two attributes; we project it
    // onto the prims that own them so a node can be reached through any of its
    // properties (a shader bridges its inputs and outputs). This is a one-shot
    // scan on a button press, not a per-frame cost.
    std::unordered_map<SdfPath, std::vector<SdfPath>, SdfPath::Hash> adjacency;
    for (const UsdPrim &prim : stage->TraverseAll()) {
        const SdfPath consumerPrim = prim.GetPath();
        for (const UsdAttribute &attr : prim.GetAttributes()) {
            if (!attr.HasAuthoredConnections()) continue;
            SdfPathVector sources;
            attr.GetConnections(&sources);
            for (const SdfPath &source : sources) {
                const SdfPath sourcePrim = source.GetPrimPath();
                if (consumerPrim == sourcePrim) continue; // skip self-loops
                adjacency[consumerPrim].push_back(sourcePrim);
                adjacency[sourcePrim].push_back(consumerPrim);
            }
        }
    }

    // Breadth-first walk from the seeds, collecting every reachable prim.
    std::unordered_set<SdfPath, SdfPath::Hash> visited;
    std::queue<SdfPath> toVisit;
    for (const UsdPrim &seed : seeds) {
        if (seed && visited.insert(seed.GetPath()).second)
            toVisit.push(seed.GetPath());
    }

    std::vector<UsdPrim> connected;
    while (!toVisit.empty()) {
        const SdfPath path = toVisit.front();
        toVisit.pop();
        if (UsdPrim prim = stage->GetPrimAtPath(path))
            connected.push_back(prim);
        const auto it = adjacency.find(path);
        if (it == adjacency.end()) continue;
        for (const SdfPath &neighbour : it->second) {
            if (visited.insert(neighbour).second)
                toVisit.push(neighbour);
        }
    }

    StageSheets *sbb = editorData.GetSheets(stage);
    if (sbb) {
        // The content of the container sheets follows their root prim, the prims are added to the default sheet
        if (sbb->GetSelectedSheet().isContainer) {
            sbb->SetSelectedSheet(defaultSheetID);
        }
        sbb->GetSelectedSheet().AddNodes(connected);
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
