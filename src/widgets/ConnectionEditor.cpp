
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

/*
 Notes on the connection editor.
    One approach was to read the whole scene at each frame to deduce/rewrite the graph of connected attributes, however after a few tests
    with the Usd and Sdf api it appears neither of them are fast enough for doing so on a moderate sized scene and low performance machine.
    So we need a different approach, potentially reading only what is needed, ideally without using caching and TfNotice
    We could add "buttons" to look for connections and keep the graph in a local memory
 */


/* 
 * Code copied from the USD repository, it extracts the shaders from a network of connected prims
 *
    UsdShadeMaterial material(prim);
    if (!material) {
        TF_RUNTIME_ERROR("Expected material prim at <%s> to be of type "
                         "'UsdShadeMaterial', not type '%s'; ignoring",
                         prim.GetPath().GetText(),
                         prim.GetTypeName().GetText());
        return VtValue();
    }

    // Bind the usd stage's resolver context for correct asset resolution.
    ArResolverContextBinder binder(prim.GetStage()->GetPathResolverContext());
    ArResolverScopedCache resolverCache;

    HdMaterialNetworkMap networkMap;

    const TfTokenVector contextVector = _GetMaterialRenderContexts();
    TfTokenVector shaderSourceTypes = _GetShaderSourceTypes();

    if (UsdShadeShader surface = material.ComputeSurfaceSource(contextVector)) {
        UsdImagingBuildHdMaterialNetworkFromTerminal(
            surface.GetPrim(), 
            HdMaterialTerminalTokens->surface,
            shaderSourceTypes,
            contextVector,
            &networkMap,
            time);

        // Only build a displacement materialNetwork if we also have a surface
        if (UsdShadeShader displacement = 
                    material.ComputeDisplacementSource(contextVector)) {
            UsdImagingBuildHdMaterialNetworkFromTerminal(
                displacement.GetPrim(),
                HdMaterialTerminalTokens->displacement,
                shaderSourceTypes,
                contextVector,
                &networkMap,
                time);
        }
    }

    // Only build a volume materialNetwork if we do not have a surface
    else if (UsdShadeShader volume = 
                    material.ComputeVolumeSource(contextVector)) {
        UsdImagingBuildHdMaterialNetworkFromTerminal(
            volume.GetPrim(),
            HdMaterialTerminalTokens->volume,
            shaderSourceTypes,
            contextVector,
            &networkMap,
            time);
    }


from pxr/usdImaging/usdImaging/materialParamUtils.cpp

using _PathSet = std::unordered_set<SdfPath, SdfPath::Hash>;

static
void _WalkGraph(
    UsdShadeConnectableAPI const & shadeNode,
    HdMaterialNetwork* materialNetwork,
    _PathSet* visitedNodes,
    TfTokenVector const & shaderSourceTypes,
    TfTokenVector const & renderContexts,
    UsdTimeCode time)
{
    // Store the path of the node
    HdMaterialNode node;
    node.path = shadeNode.GetPath();
    if (!TF_VERIFY(node.path != SdfPath::EmptyPath())) {
        return;
    }

    // If this node has already been found via another path, we do
    // not need to add it again.
    if (!visitedNodes->insert(shadeNode.GetPath()).second) {
        return;
    }

    // Visit the inputs of this node to ensure they are emitted first.
    const std::vector<UsdShadeInput> shadeNodeInputs = shadeNode.GetInputs();
    for (UsdShadeInput input: shadeNodeInputs) {

        TfToken inputName = input.GetBaseName();

        // Find the attribute this input is getting its value from, which might
        // be an output or an input, including possibly itself if not connected
        UsdShadeAttributeType attrType;
        UsdAttribute attr = input.GetValueProducingAttribute(&attrType);

        if (attrType == UsdShadeAttributeType::Output) {
            // If it is an output on a shading node we visit the node and also
            // create a relationship in the network
            _WalkGraph(UsdShadeConnectableAPI(
                attr.GetPrim()),
                materialNetwork,
                visitedNodes,
                shaderSourceTypes,
                renderContexts,
                time);

            HdMaterialRelationship relationship;
            relationship.outputId = node.path;
            relationship.outputName = inputName;
            relationship.inputId = attr.GetPrim().GetPath();
            relationship.inputName = UsdShadeOutput(attr).GetBaseName();
            materialNetwork->relationships.push_back(relationship);
        } else if (attrType == UsdShadeAttributeType::Input) {
            // If it is an input attribute we get the authored value.
            //
            // If its type is asset and contains <UDIM>,
            // we resolve the asset path with the udim pattern to a file
            // path with a udim pattern, e.g.,
            // /someDir/myImage.<UDIM>.exr to /filePath/myImage.<UDIM>.exr.
            const VtValue value = _ResolveMaterialParamValue(attr, time);
            if (!value.IsEmpty()) {
                node.parameters[inputName] = value;
            }
        }
    }

    // Extract the identifier of the node.
    // GetShaderNodeForSourceType will try to find/create an Sdr node for all
    // three info cases: info:id, info:sourceAsset and info:sourceCode.
    TfToken id = _GetNodeId(shadeNode, shaderSourceTypes, renderContexts);

    if (!id.IsEmpty()) {
        node.identifier = id;

        // GprimAdapter can filter-out primvars not used by a material to reduce
        // the number of primvars send to the render delegate. We extract the
        // primvar names from the material node to ensure these primvars are
        // not filtered-out by GprimAdapter.
        _ExtractPrimvarsFromNode(node, materialNetwork, shaderSourceTypes);
    }

    materialNetwork->nodes.push_back(node);
}



function UsdImagingBuildHdMaterialNetworkFromTerminal

    _PathSet visitedNodes;

    _WalkGraph(
        UsdShadeConnectableAPI(usdTerminal),
        &network,
        &visitedNodes, 
        shaderSourceTypes,
        renderContexts,
        time);


*/

using _PathSet = std::unordered_set<SdfPath, SdfPath::Hash>;

/*
 * Code copied from imgui node graph examples
 */

// Dummy
struct Node
{
    int     ID;
    //char    Name[32]; // TODO: don't store node name, rather store the node path
    SdfPath Path;
    ImVec2  Pos, Size;
    float   Value;
    ImVec4  Color;
    int     InputsCount, OutputsCount;

    Node(int id, const SdfPath &path, const ImVec2& pos, float value, const ImVec4& color, int inputs_count, int outputs_count) { ID = id; Path=path; Pos = pos; Value = value; Color = color; InputsCount = inputs_count; OutputsCount = outputs_count; }

    ImVec2 GetInputSlotPos(int slot_no) const { return ImVec2(Pos.x, Pos.y + Size.y * ((float)slot_no + 1) / ((float)InputsCount + 1)); }
    ImVec2 GetOutputSlotPos(int slot_no) const { return ImVec2(Pos.x + Size.x, Pos.y + Size.y * ((float)slot_no + 1) / ((float)OutputsCount + 1)); }
};


// Store the prim node graphic state: position, color, etc ...
using PrimNodesT = std::unordered_map<SdfPath, Node*, SdfPath::Hash>;

static PrimNodesT primNodes = PrimNodesT();

struct NodeLink
{
    int InputIdx, InputSlot, OutputIdx, OutputSlot;
    NodeLink(int input_idx, int input_slot, int output_idx, int output_slot) { InputIdx = input_idx; InputSlot = input_slot; OutputIdx = output_idx; OutputSlot = output_slot; }
};

// Stores the number of node per depth
static std::vector<int> nodeLayout = {};


int _WalkGraph(UsdShadeConnectableAPI const & shadeNode,
                _PathSet &visitedNodes,
                ImVector<Node*> &nodes, ImVector<NodeLink> &links, int depth) {

    if (!shadeNode) {
        return 0;
    }
    //visitedNodes.clear();
    if (!visitedNodes.insert(shadeNode.GetPath()).second) {
        return 0;
    }
    //std::cout << shadeNode.GetPath().GetString() << std::endl;
    if (nodeLayout.size()<=depth) {nodeLayout.push_back(1);}
    else {nodeLayout[depth]++;}
    int nodeId = nodes.size();
    auto nodeIt = primNodes.find(shadeNode.GetPath());
    if (nodeIt == primNodes.end()){
        primNodes[shadeNode.GetPath()] = new Node(nodeId, shadeNode.GetPath(),
                           ImVec2(-depth * 300, nodeLayout[depth]*50), 0.5f,
                           ImColor(255, 100, 100), shadeNode.GetInputs().size(), shadeNode.GetOutputs().size());
        nodes.push_back(primNodes[shadeNode.GetPath()]);
    } else {
        nodeIt->second->ID = nodeId;
        nodes.push_back(nodeIt->second);
    }


    // Visit the inputs of this node to ensure they are emitted first.
    const std::vector<UsdShadeInput> shadeNodeInputs = shadeNode.GetInputs();
    int inputIdx = 0;
    for (UsdShadeInput input: shadeNodeInputs) {
        TfToken inputName = input.GetBaseName();
        // Find the attribute this input is getting its value from, which might
        // be an output or an input, including possibly itself if not connected
        UsdShadeAttributeType attrType;
        UsdAttribute attr = input.GetValueProducingAttribute(&attrType); // DEPRECATED
        // THIS DOESN'T SHOW THE NODE GRAPHS !
        if (attrType == UsdShadeAttributeType::Output) {
            // If it is an output on a shading node we visit the node and also
            // create a relationship in the network
            int outputIdx = _WalkGraph(UsdShadeConnectableAPI(attr.GetPrim()), visitedNodes, nodes, links, depth+1);
            // TODO : link this input to the output
            if (outputIdx)
                links.push_back(NodeLink(nodeId, inputIdx, outputIdx, 0));
            
        } else if (attrType == UsdShadeAttributeType::Input) {
            // If it is an input attribute we get the authored value.
            //
            // If its type is asset and contains <UDIM>,
            // we resolve the asset path with the udim pattern to a file
            // path with a udim pattern, e.g.,
            // /someDir/myImage.<UDIM>.exr to /filePath/myImage.<UDIM>.exr.
            //const VtValue value = _ResolveMaterialParamValue(attr, time);
        } else {
           // std::cout << "unknown " << attr.GetPath().GetString() << std::endl;
        }
        inputIdx++;
    }
    return nodeId;
}


void BuildNodeGraph(const UsdPrim &prim, ImVector<Node*> &nodes, ImVector<NodeLink> &links) {
    UsdShadeMaterial material(prim);
    nodeLayout.clear();
    nodes.clear(); // TODO: how do we keep the position of the nodes ???
    links.clear();
    if (!material) return;
    
    // TODO nodes for the material outputs
    // TODO: should we be able to edit the material in context ? purpose etc
    
    ///
    /// Returns a list, in descending order of preference, that can be used to
    /// select among multiple material network implementations. The default
    /// list contains an empty token.
    /// This is generally provided by the renderDelegate
    const TfTokenVector contextVector{TfToken("render"), TfToken("mtlx"), TfToken("arnold")}; //= _GetMaterialRenderContexts();
    if (UsdShadeShader surface = material.ComputeSurfaceSource(contextVector)) {
        static _PathSet visitedNodes;
        visitedNodes.clear();
        _WalkGraph(UsdShadeConnectableAPI(surface), visitedNodes, nodes, links, 0);
    }
    
}


static void ShowExampleAppCustomNodeGraph(const UsdPrim &prim)
{
    ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
    if (!prim) return;
    
    static UsdPrim lastMaterialPrim;
    UsdShadeMaterial material(prim);
    if((!lastMaterialPrim || lastMaterialPrim!=prim) && material) {
        lastMaterialPrim = prim;
    }

    // State
    static ImVector<Node*> nodes;
    static ImVector<NodeLink> links;
    static ImVec2 scrolling = ImVec2(0.0f, 0.0f);
    static bool inited = false;
    static bool show_grid = true;
    static int node_selected = -1;

    // Initialization
    ImGuiIO& io = ImGui::GetIO();
//    if (!inited)
//    {
//        nodes.push_back(Node(0, "MainTex", ImVec2(40, 50), 0.5f, ImColor(255, 100, 100), 1, 1));
//        nodes.push_back(Node(1, "BumpMap", ImVec2(40, 150), 0.42f, ImColor(200, 100, 200), 1, 1));
//        nodes.push_back(Node(2, "Combine", ImVec2(270, 80), 1.0f, ImColor(0, 200, 100), 2, 2));
//        links.push_back(NodeLink(0, 0, 2, 0));
//        links.push_back(NodeLink(1, 0, 2, 1));
//        inited = true;
//    }
    int nodeId = 0;
    //UsdShadeNodeGraph nodeGraph(prim);
    //UsdUINodeGraphNodeAPI uiNodeGraph(prim);
    // TO walk the graph one needs to use UsdShadeConnectableAPI
    //UsdShadeConnectableAPI api(prim);
//    if (nodeGraph) {
//        UsdUINodeGraphNodeAPI::Apply(prim);
//        auto nodeGraphOutputs = nodeGraph.GetOutputs();
//        nodes.clear();
//        links.clear();
//        for (const auto &output : nodeGraphOutputs) {
//            auto outputPrim = output.GetPrim();
//            // outputPrim.GetName().GetString().c_str()
//            nodes.push_back(Node(nodeId++, output.GetFullName().GetString().c_str(), ImVec2(-nodeId * 300, 50), 0.5f,
//                                 ImColor(255, 100, 100), 1, 1));
//            auto inputs = output.GetValueProducingAttributes();
//            for (const auto &inPrim : inputs) {
//                nodes.push_back(Node(nodeId++, inPrim.GetPrim().GetName().GetString().c_str(), ImVec2(-nodeId * 300, 50), 0.5f,
//                                     ImColor(255, 100, 100), 1, 1));
//            }
//        }
//    }

    BuildNodeGraph(lastMaterialPrim, nodes, links);
    // Draw a list of nodes on the left side
    bool open_context_menu = false;
    int node_hovered_in_list = -1;
    int node_hovered_in_scene = -1;

    ImGui::BeginGroup();
    
    if(lastMaterialPrim) {
        ImGui::Text("%s", lastMaterialPrim.GetPath().GetText());
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    }


    const float NODE_SLOT_RADIUS = 4.0f;
    const ImVec2 NODE_WINDOW_PADDING(8.0f, 8.0f);

    // Create our child canvas
    //ImGui::Text("Hold middle mouse button to scroll (%.2f,%.2f)", scrolling.x, scrolling.y);

    ImGui::Checkbox("Show grid", &show_grid);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(60, 60, 70, 200));
    ImGui::BeginChild("scrolling_region", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(); // WindowPadding
    ImGui::PushItemWidth(120.0f);

    const ImVec2 offset = ImGui::GetCursorScreenPos() + scrolling;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Display grid
    if (show_grid) {
        ImU32 GRID_COLOR = IM_COL32(200, 200, 200, 40);
        float GRID_SZ = 64.0f;
        ImVec2 win_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_sz = ImGui::GetWindowSize();
        for (float x = fmodf(scrolling.x, GRID_SZ); x < canvas_sz.x; x += GRID_SZ)
            draw_list->AddLine(ImVec2(x, 0.0f) + win_pos, ImVec2(x, canvas_sz.y) + win_pos, GRID_COLOR);
        for (float y = fmodf(scrolling.y, GRID_SZ); y < canvas_sz.y; y += GRID_SZ)
            draw_list->AddLine(ImVec2(0.0f, y) + win_pos, ImVec2(canvas_sz.x, y) + win_pos, GRID_COLOR);
    }

    // Display links
    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(0); // Background
    for (int link_idx = 0; link_idx < links.Size; link_idx++)
    {
        NodeLink* link = &links[link_idx];
        Node* node_inp = nodes[link->InputIdx];
        Node* node_out = nodes[link->OutputIdx];
        ImVec2 p1 = offset + node_inp->GetInputSlotPos(link->InputSlot);
        ImVec2 p2 = offset + node_out->GetOutputSlotPos(link->OutputSlot);
        draw_list->AddBezierCubic(p1, p1 + ImVec2(-50, 0), p2 + ImVec2(+50, 0), p2, IM_COL32(200, 200, 100, 255), 3.0f);
    }

    // Display nodes
    for (int node_idx = 0; node_idx < nodes.Size; node_idx++)
    {
        Node* node = nodes[node_idx];
        ImGui::PushID(node->ID);
        ImVec2 node_rect_min = offset + node->Pos;

        // Display node contents first
        draw_list->ChannelsSetCurrent(1); // Foreground
        bool old_any_active = ImGui::IsAnyItemActive();
        ImGui::SetCursorScreenPos(node_rect_min + NODE_WINDOW_PADDING);
        ImGui::BeginGroup(); // Lock horizontal position
        UsdPrim nodePrim = prim.GetStage()->GetPrimAtPath(node->Path);
        ImGui::Text("%s", nodePrim.GetName().GetText());


        //UsdShadeConnectableAPI shader(nodePrim);
        const auto infoIdAttr = nodePrim.GetAttribute(TfToken("info:id"));
        if (infoIdAttr) {
            VtValue infoIdVal;
            infoIdAttr.Get(&infoIdVal, SdfTimeCode());
            ImGui::Text("%s", infoIdVal.Get<TfToken>().GetText());
        }
        //ImGui::SliderFloat("##value", &node->Value, 0.0f, 1.0f, "Alpha %.2f");
        //ImGui::ColorEdit3("##color", &node->Color.x);
//        for(int i=0; i<std::max(node->InputsCount, node->OutputsCount); ++i) {
//            ImGui::Text("");
//        }
        ImGui::EndGroup();

        // Save the size of what we have emitted and whether any of the widgets are being used
        bool node_widgets_active = (!old_any_active && ImGui::IsAnyItemActive());
        node->Size = ImGui::GetItemRectSize() + NODE_WINDOW_PADDING + NODE_WINDOW_PADDING;
        ImVec2 node_rect_max = node_rect_min + node->Size;

        // Display node box
        draw_list->ChannelsSetCurrent(0); // Background
        ImGui::SetCursorScreenPos(node_rect_min);
        if(ImGui::InvisibleButton("node", node->Size)){
            ExecuteAfterDraw<EditorSetSelection>(prim.GetStage(), node->Path);
        }
        if (ImGui::IsItemHovered())
        {
            node_hovered_in_scene = node->ID;
            open_context_menu |= ImGui::IsMouseClicked(1);
        }
        bool node_moving_active = ImGui::IsItemActive();
        if (node_widgets_active || node_moving_active)
            node_selected = node->ID;
        if (node_moving_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            node->Pos = node->Pos + io.MouseDelta;
        ImU32 node_bg_color = (node_hovered_in_list == node->ID || node_hovered_in_scene == node->ID || (node_hovered_in_list == -1 && node_selected == node->ID)) ? IM_COL32(75, 75, 75, 255) : IM_COL32(60, 60, 60, 255);
        draw_list->AddRectFilled(node_rect_min, node_rect_max, node_bg_color, 4.0f);
        draw_list->AddRect(node_rect_min, node_rect_max, IM_COL32(100, 100, 100, 255), 4.0f);
        for (int slot_idx = 0; slot_idx < node->InputsCount; slot_idx++)
            draw_list->AddCircleFilled(offset + node->GetInputSlotPos(slot_idx), NODE_SLOT_RADIUS, IM_COL32(150, 150, 150, 150));
        for (int slot_idx = 0; slot_idx < node->OutputsCount; slot_idx++)
            draw_list->AddCircleFilled(offset + node->GetOutputSlotPos(slot_idx), NODE_SLOT_RADIUS, IM_COL32(150, 150, 150, 150));

        ImGui::PopID();
    }
    draw_list->ChannelsMerge();

    // Open context menu
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) || !ImGui::IsAnyItemHovered())
        {
            node_selected = node_hovered_in_list = node_hovered_in_scene = -1;
            open_context_menu = true;
        }
    if (open_context_menu)
    {
        ImGui::OpenPopup("context_menu");
        if (node_hovered_in_list != -1)
            node_selected = node_hovered_in_list;
        if (node_hovered_in_scene != -1)
            node_selected = node_hovered_in_scene;
    }

    // Draw context menu
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginPopup("context_menu"))
    {
        Node* node = node_selected != -1 ? nodes[node_selected] : NULL;
        ImVec2 scene_pos = ImGui::GetMousePosOnOpeningCurrentPopup() - offset;
        if (node)
        {
            //ImGui::Text("Node '%s'", node->Name);
            ImGui::Separator();
            if (ImGui::MenuItem("Rename..", NULL, false, false)) {}
            if (ImGui::MenuItem("Delete", NULL, false, false)) {}
            if (ImGui::MenuItem("Copy", NULL, false, false)) {}
        }
        else
        {
            //if (ImGui::MenuItem("Add")) { nodes.push_back(Node(nodes.Size, "New node", scene_pos, 0.5f, ImColor(100, 100, 200), 2, 2)); }
      //      if (ImGui::MenuItem("Paste", NULL, false, false)) {}
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();


    // Scrolling
    if (ImGui::IsWindowHovered()
        //&& !ImGui::IsAnyItemActive()
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)
        && ImGui::IsKeyDown(ImGuiKey_Space)) {
            scrolling = scrolling + io.MouseDelta;
    }

    ImGui::PopItemWidth();
    ImGui::EndChild(); // Scrolling region
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndGroup();

}

PXR_NAMESPACE_USING_DIRECTIVE

// TODO node selection (multi)


///
///
///             NEW VERSION WORK IN PROGRESS
///
///


// TODO: Do we want undo/redo on the node editor ? moving nodes, deleting, etc ?


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
    std::vector<std::string> properties;
    bool selected = false;
};

// TODO: what do we need to draw a bezier curve between nodes
// TODO: how is the interaction with bezier curves done ??
struct NodeConnection {
    ImVec2 begin;   // 8 bytes
    ImVec2 end;     // 8 bytes
};

// TODO black board sheet should also store the canvas position, that would be easier when switching

struct ConnectionsSheet {
    
    // Should we keep the root prim ?? the root path under which we would create new prims
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
    
    // Update the node inputs/outputs positions and the connections such that this is visually
    // coherent
    void Update() {
        // For all the nodes, update positions and connections and attributes ??
        // We need to know the stage
        static std::vector<std::pair<SdfPath, SdfPath>> connections;
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
                node.properties.push_back(attr.GetName().GetString());
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
        std::cout << "Connections " << connections.size() << std::endl;
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

    void Begin(ImDrawList* drawList_) {
        drawList = drawList_;
        ImGuiContext& g = *GImGui;
        // Current widget position, in absolute coordinates. (relative to the main window)
        widgetOrigin = ImGui::GetCursorScreenPos(); // canvasOrigin, canvasSize
        // WindowSize returns the size of the whole window, including tabs that we have to remove to get the widget size.
        // GetCursorPos gives the current position in window coordinates
        widgetSize = ImGui::GetWindowSize() - ImGui::GetCursorPos() - g.Style.WindowPadding; // Should also add the borders
        originOffset = (widgetSize / 2.f); // in window Coordinates
        drawList->ChannelsSplit(2);
        // TODO: we might want to use only widgetBB and get rid of widgetOrigin and widgetSize
        widgetBoundingBox.Min = widgetOrigin;
        widgetBoundingBox.Max = widgetOrigin + widgetSize;
        drawList->PushClipRect(widgetBoundingBox.Min, widgetBoundingBox.Max);
    }
    
    void End() {
        drawList->ChannelsMerge();
        drawList->PopClipRect();
    };

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
        gridOrigin = ImVec2(std::ceilf(gridOrigin.x/gridSpacing)*gridSpacing, std::ceilf(gridOrigin.y/gridSpacing)*gridSpacing);
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
    
    void DrawNode(UsdPrimNode &node) {
        ImGuiContext& g = *GImGui;
        drawList->ChannelsSetCurrent(1); // Foreground
        constexpr float headerHeight = 50.f;
        constexpr float connectorSize = 14.f; // square connector size

        //static std::vector<UsdAttribute> properties;
        //properties.clear();
        const std::vector<std::string> &properties = node.properties;
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
        
        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), 0xFF090920, 4.0f);
        drawList->AddRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), node.selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 0, 255), 4.0f);

        // TODO: add padding, truncate name if too long, add tooltip
        drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(nodeMin), IM_COL32(255, 255, 255, 255), node.prim.GetName().GetText());
        
        // Draw properties
        const ImVec2 inputStartPos = nodeMin + ImVec2(10.f, headerHeight);
        for (int i=0; i<properties.size(); i++) {
            const float linePos = i * (g.FontSize + 2.f); // 2.f == padding
            
            // Input connector position min max
            const auto conMin = ImVec2(nodeMin.x - connectorSize/2, inputStartPos.y + 1.f + linePos);
            const auto conMax = ImVec2(nodeMin.x + connectorSize/2, inputStartPos.y + 15.f + linePos);
            drawList->AddRectFilled(CanvasToScreen(conMin), CanvasToScreen(conMax), IM_COL32(255, 255, 255, 255));
            
            // Output connector min/max
            const auto outConMin = ImVec2(nodeMax.x - connectorSize/2, inputStartPos.y + 1.f + linePos);
            const auto outConMax = ImVec2(nodeMax.x + connectorSize/2, inputStartPos.y + 15.f + linePos);
            drawList->AddRectFilled(CanvasToScreen(outConMin), CanvasToScreen(outConMax), IM_COL32(255, 255, 255, 255));
     
            
            
            const auto textPos = inputStartPos + ImVec2(0, linePos); // TODO: padding and text size
            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255), properties[i].c_str());
        }
        
        // The invisible button will trigger the sliders if it's not clipped
        ImRect buttonBoundingBox(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax));
        buttonBoundingBox.ClipWith(widgetBoundingBox);
        ImGui::SetCursorScreenPos(buttonBoundingBox.Min);
        if (buttonBoundingBox.GetArea() > 0.f && ImGui::InvisibleButton("node", buttonBoundingBox.GetSize())) {
            node.selected = !node.selected;
        }
        // TODO we should have a IsItemDragging instead, first click on the item to make it "drag"
        ImGuiIO& io = ImGui::GetIO();
        
        if (ImGui::IsItemHovered())
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                node.position = ScreenToCanvas(io.MousePos);
        }
    }
    
    void DrawConnections() {
        
    }
    
    // Draw so test nodes to see how the coordinate system works as all are expressed in screen
    void DrawNodeTest(ImVec2 &nodePos) {
        ImGuiContext& g = *GImGui;
        ImGuiIO& io = ImGui::GetIO();
        //ImGuiWindow* window = g.CurrentWindow;
        // Give the node a position in canvas coordinates
        drawList->ChannelsSetCurrent(1); // Foreground
        
        // Node is centered here, is it a good idea in the end ??
        int nbNodes = 8;
        
        const float headerHeight = 50.f;
        const float conSize = 14.f; // square connector size
        // Compute node size (node height)
        float nodeHeight = (nbNodes+2)*(g.FontSize + 2.f) + headerHeight; // 50 == header size
        
        // Node bounding box
        ImVec2 nodeMin = ImVec2(-80.f, -nodeHeight/2.f) + nodePos;
        ImVec2 nodeMax = ImVec2(80.f, nodeHeight/2.f) + nodePos;
        
        drawList->AddRectFilled(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), 0xFF090920, 4.0f);
        drawList->AddRect(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax), IM_COL32(255, 255, 255, 255), 4.0f);

        // TODO: add padding, truncate name if too long, add tooltip
        drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(nodeMin), IM_COL32(255, 255, 255, 255), "Node test");

        // Draw inputs
        const char *inputs[5] = {"size", "color", "height", "width", "radius"};
        
        // Start of the input list
        const ImVec2 inputStartPos = nodeMin + ImVec2(10.f, headerHeight);
        for (int i=0; i<5; i++) {
            const float linePos = i * (g.FontSize + 2.f); // 2.f == padding
            // Connector position min max
            const auto conMin = ImVec2(nodeMin.x - conSize/2, inputStartPos.y + 1.f + linePos);
            const auto conMax = ImVec2(nodeMin.x + conSize/2, inputStartPos.y + 15.f + linePos);
            drawList->AddRectFilled(CanvasToScreen(conMin), CanvasToScreen(conMax), IM_COL32(255, 255, 255, 255));
            
            const auto textPos = inputStartPos + ImVec2(0, linePos); // TODO: padding and text size
            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255), inputs[i]);
        }
        
        // Draw outputs, outputs start after the inputs
        const char *outputs[3] = {"output", "material", "shader"};
        const ImVec2 outputStartPos = nodeMin + ImVec2(10.f, headerHeight) + ImVec2(80.f, 0.f);
        for (int i=0; i<3; i++) {
            const float linePos = (i+6) * (g.FontSize + 2.f); // 2.f == padding, 6 == number of input nodes +1
            // Connector position min max
            const auto conMin = ImVec2(nodeMax.x - conSize/2, inputStartPos.y + 1.f + linePos);
            const auto conMax = ImVec2(nodeMax.x + conSize/2, inputStartPos.y + 15.f + linePos);
            drawList->AddRectFilled(CanvasToScreen(conMin), CanvasToScreen(conMax), IM_COL32(255, 255, 255, 255));
            
            const auto textPos = outputStartPos + ImVec2(0, linePos); // TODO: padding and text size
            drawList->AddText(g.Font, g.FontSize*zooming, CanvasToScreen(textPos), IM_COL32(255, 255, 255, 255), outputs[i]);
        }
        
        // The invisible button will trigger the sliders
        ImRect buttonBoundingBox(CanvasToScreen(nodeMin), CanvasToScreen(nodeMax));
        buttonBoundingBox.ClipWith(widgetBoundingBox);
        
        //drawList->AddRect(buttonBoundingBox.Min, buttonBoundingBox.Max, IM_COL32(255, 0, 0, 255), 4.0f);
        
        ImGui::SetCursorScreenPos(buttonBoundingBox.Min);
        if (buttonBoundingBox.GetArea() > 0.f && ImGui::InvisibleButton("node", buttonBoundingBox.GetSize())) {
        }
        // TODO we should have a IsItemDragging instead, first click on the item to make it "drag"
        
        if (ImGui::IsItemHovered())
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                nodePos = nodePos + io.MouseDelta/zooming;
        }
    }
    
    void DrawNodeTest() {
        static ImVec2 nodePos(0.f, 0.f); // Position in the canvas
        DrawNodeTest(nodePos);
    }
    
    void DrawSheet(ConnectionsSheet &sheet) {
        ImGuiContext& g = *GImGui;
        ImGuiIO& io = ImGui::GetIO();
        //ImGuiWindow* window = g.CurrentWindow;
        // Give the node a position in canvas coordinates
        drawList->ChannelsSetCurrent(1); // Foreground
        //static ImVec2 nodePos(0.f, 0.f); // Position in the canvas
        for (auto &node : sheet.nodes) {
            DrawNode(node);
        }
    }
    
    // Should we store sheet in the canvas directly ?? if it is used for storing scrolling and pos
    void HandleManipulation(ConnectionsSheet &sheet) {
        ImGuiIO& io = ImGui::GetIO();
        if (//&& !ImGui::IsAnyItemActive()
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            if (ImGui::IsKeyDown(ImGuiKey_Space)) {
                scrolling = scrolling + io.MouseDelta;
            } else if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                // We should zoom with the origin at the click
                ImVec2 clickedAt = io.MouseClickedPos[ImGuiMouseButton_Left];
                ZoomFromPosition(clickedAt, io.MouseDelta);
            }
        }
    }
    
    // canvasToScreen = canvasToWindow * windowToScreen;
    
    // We want the origin to be at the center of the canvas
    // so we apply an offset which depends on the canvas size
    ImVec2 originOffset = ImVec2(0.0f, 0.0f);
    ImVec2 scrolling = ImVec2(0.0f, 0.0f); // expressed in screen space
    // Zooming starting at 1 means we have an equivalence between screen and canvas unit, this
    // is probably not what we want.
    // Zooming 10 means 10 times bigger than the pixel size
    float zooming = 1.f; // TODO: make sure zooming is never 0
    
    ImVec2 widgetOrigin = ImVec2(0.0f, 0.0f);  // canvasOrigin, canvasSize in screen coordinates
    ImVec2 widgetSize = ImVec2(0.0f, 0.0f);
    ImRect widgetBoundingBox;
    ImDrawList* drawList = nullptr;
};


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
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        canvas.Begin(drawList);
        canvas.DrawGrid();
        canvas.DrawSheet(sheet);
        //canvas.DrawBoundaries(); // for debugging
        //canvas.DrawNodeTest(); // for debugging
        if (ImGui::IsWindowHovered()) {
            canvas.HandleManipulation(sheet);
        }
        canvas.End();
    }
}

void DrawConnectionEditor(const UsdPrim &prim){
    ShowExampleAppCustomNodeGraph(prim);
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
    if (!prims.empty()) {
        // Assuming all the prims are coming from the same stage
        StageSheets *sbb = editorData.GetSheets(prims[0].GetStage());
        if (sbb) {
            ConnectionsSheet &bbs = sbb->GetSelectedSheet();
            bbs.AddNodes(prims);
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
