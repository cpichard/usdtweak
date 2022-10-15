
#include "ConnectionEditor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "Commands.h"
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>
#include <iostream>

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
        draw_list->AddBezierCurve(p1, p1 + ImVec2(-50, 0), p2 + ImVec2(+50, 0), p2, IM_COL32(200, 200, 100, 255), 3.0f);
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
            ExecuteAfterDraw<EditorSetSelected>(prim.GetStage(), node->Path);
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
            if (ImGui::MenuItem("Paste", NULL, false, false)) {}
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

void DrawConnectionEditor(const UsdPrim &prim){
    ShowExampleAppCustomNodeGraph(prim);
}
