#include "UsdTools.h"

#include "JsHelpers.h"

namespace UsdAgent {

namespace {

// Build one tool definition in the neutral shape (see LLMBackend.h).
JsObject _Tool(const std::string& name,
               const std::string& description,
               const JsObject&    properties,
               const JsArray&     required) {
    JsObject schema;
    schema["type"]       = JsValue(std::string("object"));
    schema["properties"] = JsValue(properties);
    schema["required"]   = JsValue(required);

    JsObject tool;
    tool["name"]        = JsValue(name);
    tool["description"] = JsValue(description);
    tool["parameters"]  = JsValue(schema);
    return tool;
}

JsArray _Strings(std::initializer_list<const char*> items) {
    JsArray a;
    for (const char* s : items) a.push_back(JsValue(std::string(s)));
    return a;
}

// Build a JSON Schema "array of strings" parameter.
JsObject _ArrayOfStringsParam(const std::string& description) {
    JsObject items;
    items["type"] = JsValue(std::string("string"));

    JsObject p;
    p["type"]        = JsValue(std::string("array"));
    p["items"]       = JsValue(items);
    p["description"] = JsValue(description);
    return p;
}

} // namespace

ToolDefs BuildReadOnlyToolDefinitions() {
    ToolDefs tools;

    // 1. get_prim_info
    {
        JsObject props;
        props["path"] = MakeStringParam(
            "the SdfPath of the prim, e.g. \"/World/Hero\"");
        tools.push_back(JsValue(_Tool(
            "get_prim_info",
            "Returns the type, kind, specifier, active state, purpose, "
            "visibility, instanceable flag, child count and property names of "
            "the prim at the given SdfPath. Use when the user asks what a prim "
            "is or wants its basic metadata. Use \"/\" to inspect the "
            "pseudo-root.",
            props, _Strings({"path"}))));
    }

    // 2. get_attribute_value
    {
        JsObject props;
        props["path"]      = MakeStringParam(
            "the SdfPath of the prim that owns the attribute");
        props["attribute"] = MakeStringParam(
            "the attribute name, e.g. \"visibility\" or \"radius\"");
        props["time"]      = MakeNumberParam(
            "optional time code; omit for the default value, otherwise the "
            "value sampled (and interpolated) at this frame");
        tools.push_back(JsValue(_Tool(
            "get_attribute_value",
            "Returns the resolved value of an attribute at the given time. "
            "Use when the user asks what a value is, or wants to compare "
            "values at different frames. Returns the typed VtValue stringified "
            "and the SdfValueTypeName.",
            props, _Strings({"path", "attribute"}))));
    }

    // 3. get_value_resolution
    {
        JsObject props;
        props["path"]      = MakeStringParam(
            "the SdfPath of the prim that owns the attribute");
        props["attribute"] = MakeStringParam(
            "the attribute name to trace");
        tools.push_back(JsValue(_Tool(
            "get_value_resolution",
            "Returns the LIVRPS value-resolution trace for an attribute: "
            "every layer that holds an opinion (strongest first, with the "
            "winning opinion marked), and the composition order rule used. "
            "Use when the user asks WHY a value is what it is, or wants to "
            "know which layer set a value.",
            props, _Strings({"path", "attribute"}))));
    }

    // 4. get_composition_arcs
    {
        JsObject props;
        props["path"] = MakeStringParam("the SdfPath of the prim to inspect");
        tools.push_back(JsValue(_Tool(
            "get_composition_arcs",
            "Returns the composition arcs on the given prim — references, "
            "payloads, inherits, specializes, variants — including which "
            "layer introduced each arc and the target prim path. Also lists "
            "variant set names and current selections. Use when the user asks "
            "where a prim comes from, what it references, or what variants "
            "are available.",
            props, _Strings({"path"}))));
    }

    // 5. get_layer_stack
    {
        JsObject props;
        tools.push_back(JsValue(_Tool(
            "get_layer_stack",
            "Returns the ordered list of layers in the current stage, from "
            "strongest to weakest. Tags each layer as [root], [edit target], "
            "[session], [muted], or [readonly] when applicable. Takes no "
            "arguments. Use when the user asks about layers, the layer stack, "
            "or which layer is being edited.",
            props, JsArray{})));
    }

    // 6. list_children
    {
        JsObject props;
        props["path"]      = MakeStringParam(
            "the SdfPath of the parent prim, e.g. \"/\" for the pseudo-root");
        props["recursive"] = MakeBoolParam(
            "if true, walks descendants up to a depth limit (default false)");
        tools.push_back(JsValue(_Tool(
            "list_children",
            "Lists the immediate children of a prim, or all descendants when "
            "recursive=true (depth-capped at 5 levels). Each entry shows the "
            "prim path and its type. Use when the user asks what is inside a "
            "prim or wants to enumerate the scene hierarchy. Prefer the "
            "non-recursive form and drill down stepwise: recursive=true on a "
            "wide stage frequently overflows the global 8 KB result cap and "
            "the result will end with a '[... truncated]' marker. If you see "
            "that marker, call find_prims or descend by path instead.",
            props, _Strings({"path"}))));
    }

    // 7a. get_selection
    {
        JsObject props;
        props["scope"] = MakeStringParam(
            "optional filter — \"stage\" returns only USD prim selection on "
            "the current stage; \"layer\" returns only SDF prim selection on "
            "the current edit-target layer. Omit to get both.");
        tools.push_back(JsValue(_Tool(
            "get_selection",
            "Returns what the user currently has selected in the editor. "
            "usdtweak tracks two independent selections: a STAGE selection "
            "(UsdPrims on the composed stage) and a LAYER selection "
            "(SdfPrimSpecs in the current edit-target layer). Both are "
            "shown unless `scope` filters one out. Use this whenever the "
            "user says \"the selected prim(s)\", \"this prim\", or asks "
            "about something without naming a path.",
            props, JsArray{})));
    }

    // 7. find_prims
    {
        JsObject props;
        props["type"]    = MakeStringParam(
            "filter by prim type name, e.g. \"Mesh\", \"Camera\", \"Xform\"");
        props["kind"]    = MakeStringParam(
            "filter by Kind, e.g. \"component\", \"group\", \"assembly\"");
        props["purpose"] = MakeStringParam(
            "filter by UsdGeomImageable purpose: \"default\", \"render\", "
            "\"proxy\", or \"guide\"");
        props["active"]  = MakeBoolParam(
            "if set, only prims whose active state matches");
        tools.push_back(JsValue(_Tool(
            "find_prims",
            "Searches the entire stage for prims matching the given filters "
            "(all filters optional, combined with AND). Result is capped at "
            "50 prims; the count is reported. Long results are additionally "
            "subject to the global 8 KB cap and may end with a "
            "'[... truncated]' marker — narrow the filters if that appears. "
            "Use when the user asks for all prims of a kind, all cameras, "
            "all render-purpose prims, etc.",
            props, JsArray{})));
    }

    return tools;
}

ToolDefs BuildEditToolDefinitions() {
    ToolDefs tools;

    // 8. set_attribute
    {
        JsObject props;
        props["path"]      = MakeStringParam(
            "the SdfPath of the prim that owns the attribute");
        props["attribute"] = MakeStringParam(
            "the attribute name, e.g. \"visibility\" or \"focalLength\"");
        props["value"]     = MakeStringParam(
            "the new value as a string. The dispatcher parses it according "
            "to the attribute's declared type. Examples: bool=\"true\", "
            "int=\"42\", float=\"3.14\", token=\"invisible\", string=\"hi\". "
            "V1 supports scalar simple types only (no vec/matrix/array).");
        props["time"]      = MakeNumberParam(
            "optional time code; omit to set the default value, otherwise "
            "writes a time sample at this frame");
        tools.push_back(JsValue(_Tool(
            "set_attribute",
            "QUEUES an edit that sets an attribute value on the current edit "
            "target. The edit is applied on the next host frame, NOT during "
            "this call — so the result string only confirms the queue. To "
            "verify the change took effect, re-read with get_attribute_value "
            "in your next tool step. Goes through usdtweak's undo stack so "
            "Ctrl+Z reverts it.",
            props, _Strings({"path", "attribute", "value"}))));
    }

    // 9. set_active
    {
        JsObject props;
        props["path"]   = MakeStringParam("the SdfPath of the prim");
        props["active"] = MakeBoolParam(
            "true to activate, false to deactivate (deactivated prims are "
            "pruned from the composed stage)");
        tools.push_back(JsValue(_Tool(
            "set_active",
            "QUEUES a SetActive edit on a prim, on the current edit target. "
            "Re-read with get_prim_info to confirm. Undoable.",
            props, _Strings({"path", "active"}))));
    }

    // 10. set_variant
    {
        JsObject props;
        props["path"]       = MakeStringParam(
            "the SdfPath of the prim that owns the variant set");
        props["variantSet"] = MakeStringParam(
            "name of the variant set, e.g. \"modelingVariant\"");
        props["variant"]    = MakeStringParam(
            "name of the variant to select; pass \"\" to clear the selection");
        tools.push_back(JsValue(_Tool(
            "set_variant",
            "QUEUES a variant selection change on a prim, on the current "
            "edit target. Use get_composition_arcs first to discover variant "
            "set names and available variants. Re-read after to confirm. "
            "Undoable.",
            props, _Strings({"path", "variantSet", "variant"}))));
    }

    // 10a. select_prims
    {
        JsObject props;
        props["paths"]  = _ArrayOfStringsParam(
            "list of SdfPath strings to select, e.g. [\"/World/Hero\", "
            "\"/World/Camera\"]. Pass [] to clear the selection.");
        props["scope"]  = MakeStringParam(
            "\"stage\" (default) sets the USD prim selection on the current "
            "stage; \"layer\" sets the SDF prim selection on the current "
            "edit-target layer. Use \"layer\" only when the user explicitly "
            "asks about layer-level / SdfPrim selection.");
        props["extend"] = MakeBoolParam(
            "false (default) replaces the existing selection; true appends "
            "the given paths to it.");
        tools.push_back(JsValue(_Tool(
            "select_prims",
            "QUEUES a change to what the user has selected in the editor. "
            "Re-read with get_selection on the next step to confirm. "
            "Selection changes are NOT undoable in usdtweak's command "
            "stack — Ctrl+Z will not revert them.",
            props, _Strings({"paths"}))));
    }

    // 11. set_visibility
    {
        JsObject props;
        props["path"]       = MakeStringParam(
            "the SdfPath of the UsdGeomImageable prim");
        props["visibility"] = MakeStringParam(
            "one of \"inherited\", \"invisible\", or \"visible\" (note: USD "
            "uses \"inherited\" not \"visible\" by convention; \"visible\" "
            "is accepted as an alias)");
        tools.push_back(JsValue(_Tool(
            "set_visibility",
            "QUEUES a visibility change on an Imageable prim, on the current "
            "edit target. Convenience wrapper around set_attribute for the "
            "common case. Re-read with get_value_resolution to confirm and "
            "see which layer holds the new opinion. Undoable.",
            props, _Strings({"path", "visibility"}))));
    }

    return tools;
}

ToolDefs BuildAllToolDefinitions() {
    ToolDefs all = BuildReadOnlyToolDefinitions();
    ToolDefs edits = BuildEditToolDefinitions();
    all.insert(all.end(), edits.begin(), edits.end());
    return all;
}

} // namespace UsdAgent
