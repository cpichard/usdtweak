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

// Build a JSON Schema "array of 3 numbers" parameter (used for vec3 values).
JsObject _Vec3Param(const std::string& description) {
    JsObject items;
    items["type"] = JsValue(std::string("number"));
    JsObject p;
    p["type"]        = JsValue(std::string("array"));
    p["items"]       = JsValue(items);
    p["description"] = JsValue(description);
    return p;
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

    // 5. get_stage_info
    {
        JsObject props;
        tools.push_back(JsValue(_Tool(
            "get_stage_info",
            "Returns stage-level metadata: defaultPrim, start/end timecodes, "
            "timeCodesPerSecond, upAxis, metersPerUnit, total prim count, and "
            "layer count. Takes no arguments. Use when the user asks about the "
            "scene as a whole, its time range, coordinate system, or scale.",
            props, JsArray{})));
    }

    // 6. get_layer_stack
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

    // 7b. get_relationship_targets
    {
        JsObject props;
        props["path"] = MakeStringParam(
            "absolute SdfPath of the prim that owns the relationship");
        props["name"] = MakeStringParam(
            "relationship name, e.g. \"material:binding\" or \"proxyPrim\"");
        tools.push_back(JsValue(_Tool(
            "get_relationship_targets",
            "Returns the resolved list of target paths for a named relationship "
            "on a prim (composed view — all layers). Use when the user asks what "
            "a relationship points to, e.g. which material is bound to a mesh, "
            "or what a proxyPrim targets. To find all relationships on a prim, "
            "call get_prim_info first and look at its property list.",
            props, _Strings({"path", "name"}))));
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
            "the new value as a string, parsed according to the attribute's "
            "declared type. Formats by category:\n"
            "  Scalars — plain value: \"3.14\", \"true\", \"myToken\"\n"
            "  Vectors (float2/3/4, double2/3/4, half2/3/4, int2/3/4, "
            "color3/4f/d/h, point3/normal3/vector3 f/d/h) — "
            "space- or comma-separated numbers, brackets optional: "
            "\"1 2 3\", \"(0.5, 0.5, 0.5)\", \"[0.1, 0.2, 0.3]\"\n"
            "  Matrices (matrix2/3/4d, frame4d) — row-major flat list: "
            "4/9/16 numbers\n"
            "  Quaternions (quatf/quatd/quath) — \"w x y z\" (real first)\n"
            "  Scalar arrays (float[]/int[]/double[] etc.) — space- or "
            "comma-separated numbers: \"1 2 3 4\"\n"
            "  String/token/asset arrays — comma-separated, quotes optional: "
            "\"a, b, c\" or \"\\\"foo\\\", \\\"bar\\\"\"\n"
            "  Vec-N arrays (float3[]/color3f[] etc.) — flat list, "
            "count must be multiple of N: \"r1 g1 b1 r2 g2 b2\"");
        props["time"]      = MakeNumberParam(
            "optional time code; omit to set the default value, otherwise "
            "writes a time sample at this frame");
        props["layer_id"]  = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as "
            "returned by get_layer_stack (e.g. \"/path/to/shot.usda\" or "
            "\"<anon:asset.usda>\"). When omitted the current edit target "
            "is used. When provided the edit target is bypassed and the "
            "named layer is written to directly. The result always reports "
            "which layer was targeted.");
        tools.push_back(JsValue(_Tool(
            "set_attribute",
            "QUEUES an edit that sets an attribute value. Without layer_id "
            "it writes to the current edit target; with layer_id it writes "
            "to that specific layer regardless of the edit target — call "
            "get_layer_stack first to get valid identifiers. The result "
            "names the target layer. The edit lands on the next frame, so "
            "re-read with get_attribute_value to confirm. Undoable.",
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

    // 10b. delete_prim
    {
        JsObject props;
        props["path"]     = MakeStringParam(
            "absolute SdfPath of the prim spec to delete, e.g. \"/World/Hero\"");
        props["layer_id"] = MakeStringParam(
            "optional: identifier of the layer to delete from, as returned by "
            "get_layer_stack. Defaults to the current edit target. Use "
            "get_layer_stack to find which layer holds the spec you want to remove.");
        tools.push_back(JsValue(_Tool(
            "delete_prim",
            "QUEUES deletion of the SdfPrimSpec at the given path from a specific "
            "layer. This is a layer-level operation: if other layers hold opinions "
            "on this prim it will still appear on the composed stage after deletion. "
            "Returns an error if no spec exists at the path in the target layer. "
            "Re-read with get_prim_info to confirm. Undoable.",
            props, _Strings({"path"}))));
    }

    // 10c. create_prim
    {
        JsObject props;
        props["path"]      = MakeStringParam(
            "absolute SdfPath of the prim to create, e.g. \"/World/Hero\"");
        props["type"]      = MakeStringParam(
            "optional USD type name, e.g. \"Xform\", \"Mesh\", \"Camera\", "
            "\"Sphere\". Omit for a typeless prim.");
        props["specifier"] = MakeStringParam(
            "one of \"def\" (default), \"over\", or \"class\". Use \"def\" for "
            "concrete prims, \"over\" to author opinions without defining the "
            "prim, \"class\" for abstract base prims.");
        props["layer_id"]  = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as returned "
            "by get_layer_stack. When omitted the current edit target is used.");
        tools.push_back(JsValue(_Tool(
            "create_prim",
            "QUEUES creation of a new prim spec. Missing ancestor prims are "
            "created automatically as typeless \"over\" specs. Returns an error "
            "if a spec already exists at the given path in the target layer — "
            "use set_attribute or set_xform to modify an existing prim. Re-read "
            "with get_prim_info to confirm after the next step. Undoable.",
            props, _Strings({"path"}))));
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

    // 11. set_xform
    {
        JsObject props;
        props["path"]      = MakeStringParam("the SdfPath of a UsdGeomXformable prim");
        props["operation"] = MakeStringParam(
            "one of \"translate\", \"rotate\", or \"scale\"");
        props["value"]     = _Vec3Param(
            "array of exactly 3 numbers [x, y, z]. Translate is in scene "
            "units. Rotate is in degrees (XYZ order). Scale is a multiplier "
            "(1.0 = no scale).");
        props["time"]      = MakeNumberParam(
            "optional time code; omit for the default (static) value");
        tools.push_back(JsValue(_Tool(
            "set_xform",
            "QUEUES a translate, rotate, or scale edit on a UsdGeomXformable "
            "prim using UsdGeomXformCommonAPI. Creates the xform op if it does "
            "not already exist, so this works on freshly-created prims. Uses "
            "the current edit target. Re-read with get_attribute_value on "
            "xformOp:translate / xformOp:rotateXYZ / xformOp:scale to confirm. "
            "Undoable. Note: only works on prims that use the common "
            "translate/rotate/scale op layout — not on matrix-based xforms.",
            props, _Strings({"path", "operation", "value"}))));
    }

    // 12. set_visibility
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

    // 13. add_reference
    {
        JsObject props;
        props["path"]         = MakeStringParam(
            "absolute SdfPath of the prim that will hold the reference arc");
        props["asset_path"]   = MakeStringParam(
            "file path to the referenced USD asset, e.g. \"/assets/hero.usda\". "
            "Pass \"\" for an internal (same-layer) reference.");
        props["prim_path"]    = MakeStringParam(
            "optional: absolute prim path within the referenced asset to target. "
            "Omit to use the asset's defaultPrim.");
        props["layer_offset"] = MakeNumberParam(
            "optional time offset in frames (default 0)");
        props["layer_scale"]  = MakeNumberParam(
            "optional time scale multiplier (default 1)");
        props["layer_id"]     = MakeStringParam(
            "optional: identifier of the layer to author in, as returned by "
            "get_layer_stack. Defaults to the current edit target.");
        tools.push_back(JsValue(_Tool(
            "add_reference",
            "QUEUES adding a reference arc to a prim. If the prim has no spec "
            "in the target layer, a typeless 'over' spec is created automatically. "
            "The new arc is prepended (strongest). Re-read with get_composition_arcs "
            "to confirm. Undoable.",
            props, _Strings({"path", "asset_path"}))));
    }

    // 14. add_payload
    {
        JsObject props;
        props["path"]         = MakeStringParam(
            "absolute SdfPath of the prim that will hold the payload arc");
        props["asset_path"]   = MakeStringParam(
            "file path to the payload USD asset, e.g. \"/assets/hero.usda\".");
        props["prim_path"]    = MakeStringParam(
            "optional: absolute prim path within the asset. Omit for defaultPrim.");
        props["layer_offset"] = MakeNumberParam(
            "optional time offset in frames (default 0)");
        props["layer_scale"]  = MakeNumberParam(
            "optional time scale multiplier (default 1)");
        props["layer_id"]     = MakeStringParam(
            "optional: layer to author in (defaults to current edit target)");
        tools.push_back(JsValue(_Tool(
            "add_payload",
            "QUEUES adding a payload arc to a prim. Payloads are like references "
            "but are loaded lazily — use them for heavy geometry or assets that "
            "should be unloadable at runtime. If the prim has no spec in the "
            "target layer, a typeless 'over' is created. The arc is prepended. "
            "Re-read with get_composition_arcs to confirm. Undoable.",
            props, _Strings({"path", "asset_path"}))));
    }

    // 15. add_inherit
    {
        JsObject props;
        props["path"]        = MakeStringParam(
            "absolute SdfPath of the prim that will inherit");
        props["target_path"] = MakeStringParam(
            "absolute SdfPath of the class prim to inherit from, "
            "e.g. \"/_class_Hero\"");
        props["layer_id"]    = MakeStringParam(
            "optional: layer to author in (defaults to current edit target)");
        tools.push_back(JsValue(_Tool(
            "add_inherit",
            "QUEUES adding an inherit arc from a prim to a class prim. Inherits "
            "are the weakest composition arc but propagate opinions to all "
            "inheriting prims, making them useful for shared overrides. If the "
            "prim has no spec in the target layer, a typeless 'over' is created. "
            "Re-read with get_composition_arcs to confirm. Undoable.",
            props, _Strings({"path", "target_path"}))));
    }

    // 16. add_specialize
    {
        JsObject props;
        props["path"]        = MakeStringParam(
            "absolute SdfPath of the prim that will specialize");
        props["target_path"] = MakeStringParam(
            "absolute SdfPath of the base prim to specialize from");
        props["layer_id"]    = MakeStringParam(
            "optional: layer to author in (defaults to current edit target)");
        tools.push_back(JsValue(_Tool(
            "add_specialize",
            "QUEUES adding a specialize arc from a prim to a base prim. "
            "Specializes are weaker than inherits — they are the last arc "
            "consulted in LIVRPS. Useful for variant-like overrides where the "
            "base should not retroactively affect the specializing prim. If the "
            "prim has no spec in the target layer, a typeless 'over' is created. "
            "Re-read with get_composition_arcs to confirm. Undoable.",
            props, _Strings({"path", "target_path"}))));
    }

    // 17. add_sublayer
    {
        JsObject props;
        props["sublayer_path"] = MakeStringParam(
            "file path to insert as a sublayer, e.g. \"/shot/anim.usda\". "
            "The path is stored as-is (relative or absolute) in the layer.");
        props["layer_id"]      = MakeStringParam(
            "optional: identifier of the layer to insert the sublayer into, "
            "as returned by get_layer_stack. Defaults to the stage's root layer.");
        tools.push_back(JsValue(_Tool(
            "add_sublayer",
            "QUEUES inserting a sublayer path into a layer (prepended, i.e. "
            "strongest). The sublayer file does not need to exist yet. Returns "
            "an error if the path is already a sublayer of the target. "
            "Re-read with get_layer_stack to confirm. Undoable.",
            props, _Strings({"sublayer_path"}))));
    }

    // 18. set_relationship
    {
        JsObject props;
        props["path"]      = MakeStringParam(
            "absolute SdfPath of the prim that owns the relationship");
        props["name"]      = MakeStringParam(
            "relationship name, e.g. \"material:binding\" or \"proxyPrim\"");
        props["targets"]   = _ArrayOfStringsParam(
            "list of target SdfPath strings, e.g. [\"/World/Materials/Mat_A\"]. "
            "Pass [] to clear all targets.");
        props["operation"] = MakeStringParam(
            "how to apply the targets: \"prepend\" (default — adds as strongest "
            "opinions), \"append\" (adds as weakest opinions), or \"explicit\" "
            "(replaces all existing targets with exactly this list).");
        props["layer_id"]  = MakeStringParam(
            "optional: identifier of the layer to write to, as returned by "
            "get_layer_stack. Defaults to the current edit target.");
        tools.push_back(JsValue(_Tool(
            "set_relationship",
            "QUEUES an edit that sets or modifies the targets of a relationship "
            "on a prim. If the prim or relationship spec does not yet exist in "
            "the target layer they are created automatically. Use get_relationship_targets "
            "first to inspect current targets. The most common use case is binding "
            "a material: name=\"material:binding\", targets=[\"/World/Materials/MyMat\"]. "
            "Re-read with get_relationship_targets to confirm. Undoable.",
            props, _Strings({"path", "name", "targets"}))));
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
