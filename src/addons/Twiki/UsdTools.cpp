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

// Build a JSON Schema "array of objects" parameter with a given item shape.
JsObject _ObjectArrayParam(const JsObject&    itemProps,
                           const JsArray&     itemRequired,
                           const std::string& description) {
    JsObject itemSchema;
    itemSchema["type"]       = JsValue(std::string("object"));
    itemSchema["properties"] = JsValue(itemProps);
    itemSchema["required"]   = JsValue(itemRequired);

    JsObject p;
    p["type"]        = JsValue(std::string("array"));
    p["items"]       = JsValue(itemSchema);
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

    // get_edit_target
    {
        JsObject props;
        tools.push_back(JsValue(_Tool(
            "get_edit_target",
            "Returns the layer that is currently set as the edit target — "
            "the layer where all write operations (set_attributes, create_prims, "
            "etc.) land by default. Shows the layer name, identifier, and any "
            "[readonly] or [muted] flags. Takes no arguments. Use before "
            "issuing edits when you need to confirm which layer will be written.",
            props, JsArray{})));
    }

    // list_children
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
        props["name_pattern"] = MakeStringParam(
            "case-sensitive substring match against the prim's local name "
            "(last path element, not the full path)");
        props["name_tokens"] = _ArrayOfStringsParam(
            "case-insensitive OR match: a prim matches if its name contains "
            "ANY of these lexical tokens (as produced by get_name_vocabulary). "
            "Use this to resolve vocabulary terms to prims in one call, e.g. "
            "[\"towel\",\"plate\"]. Matches whole tokens, not substrings — for "
            "substring matching on the raw name use name_pattern instead.");
        props["under"] = _ArrayOfStringsParam(
            "restrict the search to these prims and their descendants (absolute "
            "prim paths). Pass one path to scope to a single subtree, e.g. "
            "[\"/Kitchen_set/Appliances_grp\"], or several to sweep multiple "
            "subtrees in ONE call, e.g. every Mesh under each appliance. ANDs "
            "with the other filters and with store_as, so this is the way to "
            "collect a scoped set (e.g. all Meshes under the appliances) without "
            "listing children subtree-by-subtree.");
        props["store_as"] = MakeStringParam(
            "optional handle name. If set, the FULL match set (every match, not "
            "just the 50 shown) is saved client-side under this name. Reuse it "
            "with list_id on the edit tools to act on ALL matches in one "
            "undoable command without re-listing paths, or page through it with "
            "read_list. This is the way to bulk-edit more than 50 prims.");
        tools.push_back(JsValue(_Tool(
            "find_prims",
            "Searches the stage for prims matching the given filters "
            "(type / kind / purpose / active / name_pattern / name_tokens / "
            "under, all optional, combined with AND). Use 'under' to scope the "
            "search to one or more subtrees (a prim path and its descendants) "
            "instead of the whole stage. Prefer name_pattern when the user "
            "refers to a prim by name rather than path. To answer questions "
            "about what KINDS of things are in the scene or to group prims by "
            "meaning, call get_name_vocabulary first, then pass the relevant "
            "tokens to name_tokens. The total match count is always reported "
            "even when the listing is capped at 50 prims — trust that count, "
            "not the number of lines shown. Long results are additionally "
            "subject to the global 8 KB cap and may end with a "
            "'[... truncated]' marker — narrow the filters if that appears. "
            "Use when the user asks for all prims of a kind, all cameras, "
            "all render-purpose prims, etc. To then edit ALL matches (not just "
            "the 50 shown), pass store_as and reuse the handle via list_id.",
            props, JsArray{})));
    }

    // 7b. run_query — UTQL, the expressive complement to find_prims.
    {
        JsObject props;
        props["query"] = MakeStringParam(
            "a single UTQL query string. Grammar (Stage-world subset):\n"
            "  FIND <entity> [CONNECTED [UPSTREAM|DOWNSTREAM] (TO|OF) <origin> "
            "[WITHIN n]] [IN RESULTSET \"name\"] [AT <time>] [WHERE <cond>] "
            "[RETURN <fields>|*] [ORDERED BY <field> [ASC|DESC]] [LIMIT n] "
            "[AS \"name\"]\n"
            "Only FIND is required; clauses must appear in that order; keywords "
            "are case-insensitive; string literals use \"double quotes\".\n"
            "ENTITIES (composed/Stage only in this tool): USDPRIM, USDATTRIBUTE, "
            "USDRELATIONSHIP. (SDF*/LAYER and CONTRIBUTING TO are NOT supported "
            "here — use find_prims / the get_* tools instead.)\n"
            "WHERE operators: `field OP literal` (OP = = != < <= > >=); "
            "`field LIKE \"sub\"` (substring) or `LIKE /regex/`; "
            "`field IN (\"a\",\"b\")`; `set CONTAINS \"v\"` (set/family fields); "
            "`field IS [NOT] NULL`; `PATH UNDER \"/p\"` (true namespace prefix); "
            "bare boolflag (e.g. ACTIVE). Combine with AND/OR/NOT and parens "
            "(NOT > AND > OR). Same-family predicates in one AND correlate to ONE "
            "arc.\n"
            "PRIM fields: PRIMNAME PATH PRIMTYPE(\"Mesh\") KIND(\"component\") "
            "SPECIFIER(\"def\"/\"over\"/\"class\") ACTIVE• DEPTH CHILDCOUNT "
            "ATTRIBUTECOUNT HAS_REFERENCE• HAS_PAYLOAD• HAS_VARIANT• "
            "HAS_API• HAS_TIMESAMPLES• ISINSTANCE• ISPROTOTYPE• "
            "ISINPROTOTYPE• ISINSTANCEPROXY• INSTANCEABLE• "
            "HAS_RELATIONSHIP• RELATIONSHIPS(set: CONTAINS \"material:binding\"); "
            "families REFERENCE.* PAYLOAD.* VARIANT.* (.SET/.SELECTION) and "
            "API (API CONTAINS \"PhysicsCollisionAPI\", API.COUNT).\n"
            "ATTRIBUTE fields: ATTRIBUTE.NAME ATTRIBUTE.TYPENAME "
            "ATTRIBUTE.NAMESPACE(\"primvars\") VALUE.SCALAR (resolved value; "
            "numeric/string/bool, arrays excluded so gate with NOT VALUE.ISARRAY) "
            "VALUE.ISARRAY• VALUE.ARRAYSIZE VALUE.HASTIMESAMPLES• "
            "VALUE.ISNONE• VALUE.ASSETMISSING• VARIABILITY INTERPOLATION "
            "HAS_CONNECTION• CONNECTION.SOURCE(set) CONNECTION.COUNT PATH. Use "
            "AT TIME t to pin a frame; otherwise the UI's current time is used. "
            "(• = unary bool flag.)\n"
            "RELATIONSHIP fields: RELATIONSHIP.NAME RELATIONSHIP.NAMESPACE "
            "RELATIONSHIP.TARGET RELATIONSHIP.TARGETCOUNT.\n"
            "CONNECTED TO (USDPRIM only) walks the attribute-connection graph and "
            "returns reached prims: `FIND USDPRIM CONNECTED TO \"/Looks/Mat\"` "
            "(undirected component), CONNECTED UPSTREAM OF feeds, WITHIN n bounds "
            "hops. Origin = a prim or attribute path.\n"
            "CHAINING (compose queries): end a query with AS \"name\" to cache "
            "its result for this session, then a later run_query can restrict to "
            "it with IN RESULTSET \"name\" (scan only those prims) or "
            "PATH UNDER RESULTSET \"name\" in WHERE (prims at/under them). Prefer "
            "this over re-deriving a set by hand. Both queries must be the same "
            "(Stage) world, which is always true in this tool.\n"
            "NOTE: UsdLux/UsdShade params are namespaced under inputs: — match "
            "\"inputs:intensity\", not \"intensity\". Plain geom attrs (points, "
            "visibility, purpose) are NOT namespaced.\n"
            "EXAMPLES:\n"
            "  FIND USDPRIM WHERE PRIMTYPE = \"Mesh\" AND NOT ACTIVE\n"
            "  FIND USDPRIM WHERE API CONTAINS \"MaterialBindingAPI\"\n"
            "  FIND USDPRIM WHERE ISINSTANCE\n"
            "  FIND USDATTRIBUTE WHERE ATTRIBUTE.NAME = \"inputs:intensity\" AND "
            "VALUE.SCALAR > 1000\n"
            "  FIND USDATTRIBUTE WHERE VALUE.ASSETMISSING\n"
            "  FIND USDPRIM CONNECTED UPSTREAM OF \"/Looks/Mat.outputs:surface\"\n"
            "  FIND USDPRIM WHERE KIND = \"component\" AS \"comps\"   then   "
            "FIND USDPRIM WHERE PRIMTYPE = \"Mesh\" AND PATH UNDER RESULTSET "
            "\"comps\"\n");
        props["store_as"] = MakeStringParam(
            "optional handle name. If set, the FULL result path set is saved "
            "client-side under this name (like find_prims store_as). For a "
            "USDPRIM query reuse it with list_id on the edit tools to act on all "
            "matches in one undoable command, or page it with read_list. (For "
            "attribute/relationship queries the stored paths are property paths, "
            "not directly usable by the prim edit tools.)");
        tools.push_back(JsValue(_Tool(
            "run_query",
            "Runs a UTQL query (usdtweak's query language) against the current "
            "stage and returns a result table. This is the EXPRESSIVE complement "
            "to find_prims: reach for it when the question needs criteria "
            "find_prims cannot express — resolved attribute values "
            "(VALUE.SCALAR), connection reachability (CONNECTED TO), instancing "
            "(ISINSTANCE / ISPROTOTYPE), relationship names (RELATIONSHIPS), "
            "animation (HAS_TIMESAMPLES), missing assets (VALUE.ASSETMISSING), "
            "composition predicates (REFERENCE.* / API), or boolean combinations "
            "of these. Prefer find_prims for simple type/kind/name lookups. The "
            "result reports matched/scanned counts and is capped at 50 displayed "
            "rows plus the global 8 KB cap. A compile error is recoverable — "
            "read the message, fix the query, and call again. Stage-world only.\n"
            "WHEN TO STORE THE RESULT (do not re-derive a set by hand): if the "
            "user's request implies acting on the matches, store them instead of "
            "re-listing paths. To EDIT or hand-curate the set, pass store_as and "
            "reuse the handle via list_id on the edit tools (or read_list to page "
            "it) — this is how you act on ALL matches, not just the 50 shown. To "
            "FILTER the set further in a follow-up query, end with AS \"name\" and "
            "reference it via IN RESULTSET \"name\" or PATH UNDER RESULTSET "
            "\"name\". Reach for store_as / AS proactively whenever a next step on "
            "the same prims is likely.",
            props, _Strings({"query"}))));
    }

    // 7d. read_list
    {
        JsObject props;
        props["list_id"] = MakeStringParam(
            "name of a list previously created by find_prims (store_as) or "
            "manage_lists.");
        props["offset"]  = MakeNumberParam(
            "index of the first path to return (default 0). Use the next_offset "
            "value from the previous call to page forward.");
        props["limit"]   = MakeNumberParam(
            "max paths to return this call (default and hard cap 50).");
        tools.push_back(JsValue(_Tool(
            "read_list",
            "Pages through a stored named list, printing paths[offset, "
            "offset+limit) plus the total and a next_offset hint. Use this when "
            "you must READ candidate paths to judge them (e.g. curating a "
            "semantic set like 'kitchen utensils') and the set is larger than "
            "the 50 find_prims shows. To merely EDIT a whole list you do NOT "
            "need read_list — pass list_id to the edit tool directly.",
            props, _Strings({"list_id"}))));
    }

    // 7e. manage_lists
    {
        JsObject props;
        props["operation"] = MakeStringParam(
            "one of: \"list\" (no other params; show all handles + counts); "
            "\"delete\" (needs list_id); \"create\" (needs store_as + paths; "
            "store an explicit path set — use for the few items you curated by "
            "hand, e.g. false positives to drop); \"combine\" (needs op + "
            "inputs + store_as; set algebra over existing lists).");
        props["op"] = MakeStringParam(
            "for operation=combine: \"union\" (merge several searches), "
            "\"intersect\" (paths in ALL inputs — AND two criteria), or "
            "\"difference\" (inputs[0] minus the rest — curation/exclusion).");
        props["inputs"]   = _ArrayOfStringsParam(
            "for operation=combine: names of the lists to combine (>= 2).");
        props["paths"]    = _ArrayOfStringsParam(
            "for operation=create: SdfPath strings to store (de-duplicated).");
        props["store_as"] = MakeStringParam(
            "for operation=create/combine: name to store the resulting list "
            "under.");
        props["list_id"]  = MakeStringParam(
            "for operation=delete: name of the list to remove.");
        tools.push_back(JsValue(_Tool(
            "manage_lists",
            "Manage the client-side named prim lists: inventory (list), remove "
            "(delete), create one from explicit paths (create), or derive a new "
            "one by set algebra over existing lists (combine). Combine keeps "
            "paths client-side, so prefer it over re-listing paths yourself: "
            "union to merge several semantic searches, difference to drop a "
            "small set of curated false positives, intersect to AND two "
            "filters. Lists are sets (no duplicates) and live for the session.",
            props, _Strings({"operation"}))));
    }

    // 7c. get_name_vocabulary
    {
        JsObject props;   // no parameters
        tools.push_back(JsValue(_Tool(
            "get_name_vocabulary",
            "Returns the lexical vocabulary of the stage: every distinct token "
            "found in prim names, with the number of prims contributing each, "
            "sorted by frequency. Names are split on '_'/'-'/camelCase, "
            "lowercased, and pure-number parts dropped (e.g. /Kitchen_001/"
            "Props/TOwel_1 contributes kitchen, props, towel). Use this as a "
            "semantic table of contents BEFORE answering questions about what "
            "is in the scene or grouping prims by meaning, then resolve tokens "
            "to prims with find_prims name_tokens. Much cheaper than "
            "enumerating the hierarchy.",
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

    // find_usd_files
    {
        JsObject props;
        props["name_pattern"] = MakeStringParam(
            "case-insensitive substring matched against the filename "
            "(including extension), e.g. \"oscilloscope\" matches "
            "\"Oscilloscope_v2.usda\". Either name_pattern or "
            "content_pattern (or both) must be provided.");
        props["content_pattern"] = MakeStringParam(
            "case-insensitive substring to search inside file contents. "
            "Only text USD files (.usda and .usd files that are not binary) "
            "are searched; .usdc (binary Crate) and .usdz (zip) are skipped.");
        props["directories"] = _ArrayOfStringsParam(
            "absolute directory paths to search. If omitted, the parent "
            "directory of the current stage's root layer is used.");
        props["recursive"] = MakeBoolParam(
            "if true (default) descend into subdirectories");
        tools.push_back(JsValue(_Tool(
            "find_usd_files",
            "Searches directories for USD-compatible files (.usd/.usda/.usdc/"
            ".usdz) matching name_pattern and/or whose text contents match "
            "content_pattern. Directories are walked in parallel; content "
            "grep is also parallelised across matching files. "
            "If directories is omitted the parent folder of the current "
            "stage is used. Results are capped at 100 files. "
            "Use for queries like \"find the oscilloscope asset in "
            "/Users/cyril/Assets\" (name search) or \"find files containing "
            "television\" (content search). Returns matching paths; for "
            "content searches also shows the first matching lines.",
            props, JsArray{})));
    }

    return tools;
}

ToolDefs BuildEditToolDefinitions() {
    ToolDefs tools;

    // 8. set_attributes
    {
        const std::string valueFormatNote =
            "string, parsed according to the attribute's declared type. "
            "Formats by category:\n"
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
            "count must be multiple of N: \"r1 g1 b1 r2 g2 b2\"";

        JsObject itemProps;
        itemProps["path"]      = MakeStringParam(
            "SdfPath of the prim that owns the attribute (required per item)");
        itemProps["attribute"] = MakeStringParam(
            "optional: attribute name. Overrides the top-level \"attribute\". "
            "Required if no top-level default is set.");
        itemProps["value"]     = MakeStringParam(
            "optional: " + valueFormatNote
            + "\nOverrides the top-level \"value\". Required if no top-level "
            "default is set.");
        itemProps["time"]      = MakeNumberParam(
            "optional time code for this item; overrides the top-level "
            "\"time\". Omit (here and at top) for the default value.");

        JsObject props;
        props["items"]     = _ObjectArrayParam(itemProps, _Strings({"path"}),
            "list of attribute edits to apply. Each entry requires \"path\"; "
            "\"attribute\", \"value\", and \"time\" may be set per-item or "
            "inherited from the top level. All entries land on the same "
            "target layer in a single undoable step.");
        props["attribute"] = MakeStringParam(
            "optional top-level default attribute name applied to every item "
            "that does not set its own. Use this for bulk edits of the same "
            "attribute (e.g. setting \"visibility\" on many prims).");
        props["value"]     = MakeStringParam(
            "optional top-level default value applied to every item that "
            "does not set its own. Use this for bulk edits with a shared "
            "value (e.g. \"invisible\" or \"0.5\"). " + valueFormatNote);
        props["time"]      = MakeNumberParam(
            "optional top-level default time code. Items without their own "
            "\"time\" inherit this; omit at both levels for the default "
            "(static) value.");
        props["layer_id"]  = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as "
            "returned by get_layer_stack (e.g. \"/path/to/shot.usda\" or "
            "\"<anon:asset.usda>\"). Applies to every entry. Omit to use "
            "the current edit target.");
        props["list_id"]   = MakeStringParam(
            "optional: instead of \"items\", apply to EVERY path in this stored "
            "list (created by find_prims store_as or manage_lists). Mutually "
            "exclusive with \"items\". The top-level \"attribute\"/\"value\"/"
            "\"time\" defaults supply the values for every path — set them. This "
            "is how you edit more than the 50 prims find_prims shows.");
        tools.push_back(JsValue(_Tool(
            "set_attributes",
            "QUEUES one or more attribute-value edits in a single call — "
            "always prefer this over multiple sequential calls. Top-level "
            "\"attribute\", \"value\", and \"time\" act as defaults; each "
            "item may override them or supply its own. Common patterns:\n"
            "  • Uniform: set top-level attribute+value, items list only "
            "paths (e.g. make many prims invisible).\n"
            "  • Heterogeneous: each item supplies its own attribute and "
            "value.\n"
            "  • Mixed: top-level attribute, per-item values.\n"
            "Per-item validation errors (no prim, no attribute, parse "
            "failure) are reported in the result and do not block the "
            "other items. All items land on the same target layer in a "
            "single undoable command. Re-read with get_attribute_value to "
            "confirm. Supply either \"items\" or \"list_id\".",
            props, JsArray{})));
    }

    // 9. set_actives
    {
        const std::string activeNote =
            "true to activate, false to deactivate (deactivated prims, and "
            "their descendants, are pruned from the composed stage).";

        JsObject itemProps;
        itemProps["path"]   = MakeStringParam(
            "SdfPath of the prim (required per item)");
        itemProps["active"] = MakeBoolParam(
            "optional: " + activeNote + " Overrides the top-level \"active\". "
            "Required if no top-level default is set.");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps, _Strings({"path"}),
            "list of active-state edits. Each entry requires \"path\"; "
            "\"active\" may be set per-item or inherited from the top level. "
            "All entries land on the same target layer in a single undoable "
            "step.");
        props["active"]   = MakeBoolParam(
            "optional top-level default applied to every item that does not set "
            "its own. Use for bulk activate/deactivate. " + activeNote);
        props["layer_id"] = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as returned "
            "by get_layer_stack. Applies to every entry. Omit to use the "
            "current edit target.");
        props["list_id"]  = MakeStringParam(
            "optional: instead of \"items\", apply to EVERY path in this stored "
            "list (from find_prims store_as / manage_lists). Mutually exclusive "
            "with \"items\". The top-level \"active\" default is applied to "
            "every path — set it (e.g. false to deactivate the whole set).");
        tools.push_back(JsValue(_Tool(
            "set_actives",
            "QUEUES one or more SetActive edits in a single call — always prefer "
            "this over multiple sequential calls. Top-level \"active\" acts as a "
            "default; each item may override it. Per-item errors (no prim) are "
            "reported and do not block the others. All items land on the same "
            "target layer in a single undoable command. Re-read with "
            "get_prim_info to confirm. Supply either \"items\" or \"list_id\".",
            props, JsArray{})));
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

    // 10b. delete_prims
    {
        JsObject itemProps;
        itemProps["path"] = MakeStringParam(
            "absolute SdfPath of the prim spec to delete, e.g. \"/World/Hero\" "
            "(required per item)");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps, _Strings({"path"}),
            "list of prim specs to delete. Each entry requires \"path\". All "
            "entries are removed from the same target layer in a single "
            "undoable step.");
        props["layer_id"] = MakeStringParam(
            "optional: identifier of the layer to delete from, as returned by "
            "get_layer_stack. Applies to every entry. Defaults to the current "
            "edit target. Use get_layer_stack to find which layer holds the "
            "specs you want to remove.");
        props["list_id"]  = MakeStringParam(
            "optional: instead of \"items\", delete EVERY path in this stored "
            "list (from find_prims store_as / manage_lists). Mutually exclusive "
            "with \"items\". Use this to bulk-prune a found/curated set.");
        tools.push_back(JsValue(_Tool(
            "delete_prims",
            "QUEUES deletion of one or more SdfPrimSpecs from a single layer in "
            "one undoable command — always prefer this over multiple sequential "
            "deletions. This is a layer-level operation: if other layers hold "
            "opinions on a prim it will still appear on the composed stage after "
            "deletion. Per-item errors (no spec in the target layer, invalid "
            "path) are reported and do not block the others. Re-read with "
            "get_prim_info to confirm. Supply either \"items\" or \"list_id\".",
            props, JsArray{})));
    }

    // 10c. create_prims
    {
        JsObject itemProps;
        itemProps["path"]      = MakeStringParam(
            "absolute SdfPath of the prim to create, e.g. \"/World/Hero_001\"");
        itemProps["type"]      = MakeStringParam(
            "optional USD type name, e.g. \"Xform\", \"Mesh\", \"Camera\", "
            "\"Sphere\". Omit for a typeless prim.");
        itemProps["specifier"] = MakeStringParam(
            "one of \"def\" (default), \"over\", or \"class\". Use \"def\" for "
            "concrete prims, \"over\" to author opinions without defining the "
            "prim, \"class\" for abstract base prims.");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps, _Strings({"path"}),
            "list of prim specs to create. Each entry has a required \"path\" "
            "and optional \"type\" and \"specifier\". All entries are authored "
            "into the same target layer in a single undoable step.");
        props["layer_id"] = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as returned "
            "by get_layer_stack. Applies to every entry in \"items\". When "
            "omitted the current edit target is used.");
        tools.push_back(JsValue(_Tool(
            "create_prims",
            "QUEUES creation of one or more prim specs in a single call — "
            "always prefer this over multiple sequential creations when adding "
            "more than one prim, including when creating instances. Missing "
            "ancestor prims are created automatically as typeless \"over\" "
            "specs. Items are processed in order; if an item fails (e.g. a "
            "spec already exists at that path, or a duplicate path appears in "
            "the batch) the others still proceed and per-item status is "
            "reported in the result. All items land on the same target layer "
            "in one undoable command. Re-read with list_children to confirm.",
            props, _Strings({"items"}))));
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
        props["list_id"] = MakeStringParam(
            "optional: instead of \"paths\", select EVERY path in this stored "
            "list (from find_prims store_as / manage_lists). Mutually exclusive "
            "with \"paths\". Handy to let the user visually confirm a found or "
            "curated set in the viewport before editing it.");
        tools.push_back(JsValue(_Tool(
            "select_prims",
            "QUEUES a change to what the user has selected in the editor. "
            "Provide either \"paths\" or \"list_id\". Re-read with "
            "get_selection on the next step to confirm. Selection changes are "
            "NOT undoable in usdtweak's command stack — Ctrl+Z will not revert "
            "them.",
            props, JsArray{})));
    }

    // set_edit_target
    {
        JsObject props;
        props["layer_id"] = MakeStringParam(
            "identifier of the layer to set as edit target, exactly as "
            "returned by get_layer_stack (e.g. \"/path/to/shot.usda\" or "
            "\"<anon:asset.usda>\"). The layer must be in the current "
            "layer stack and must not be read-only or muted.");
        tools.push_back(JsValue(_Tool(
            "set_edit_target",
            "QUEUES changing the stage's edit target to the named layer. "
            "After the change, all write operations (set_attributes, "
            "create_prims, etc.) that do not specify an explicit layer_id "
            "will write to this layer. Re-read with get_edit_target on your "
            "next step to confirm. NOT undoable — the edit target is "
            "session state, not a layer opinion.",
            props, _Strings({"layer_id"}))));
    }

    // set_xforms
    {
        const std::string valueFormatNote =
            "array of exactly 3 numbers [x, y, z]. Translate is in scene "
            "units. Rotate is in degrees (XYZ order). Scale is a multiplier "
            "(1.0 = no scale).";

        JsObject itemProps;
        itemProps["path"]      = MakeStringParam(
            "SdfPath of a UsdGeomXformable prim (required per item)");
        itemProps["operation"] = MakeStringParam(
            "optional: one of \"translate\", \"rotate\", or \"scale\". "
            "Overrides the top-level \"operation\". Required if no top-level "
            "default is set.");
        itemProps["value"]     = _Vec3Param(
            "optional: " + valueFormatNote
            + " Overrides the top-level \"value\". Required if no top-level "
            "default is set.");
        itemProps["time"]      = MakeNumberParam(
            "optional time code for this item; overrides the top-level "
            "\"time\". Omit (here and at top) for the default (static) value.");

        JsObject props;
        props["items"]     = _ObjectArrayParam(itemProps, _Strings({"path"}),
            "list of xform edits to apply. Each entry requires \"path\"; "
            "\"operation\", \"value\", and \"time\" may be set per-item or "
            "inherited from the top level. All entries land on the same "
            "target layer in a single undoable step.");
        props["operation"] = MakeStringParam(
            "optional top-level default operation applied to every item that "
            "does not set its own. Use for bulk edits with a shared op "
            "(e.g. translating many prims).");
        props["value"]     = _Vec3Param(
            "optional top-level default value applied to every item that "
            "does not set its own. " + valueFormatNote);
        props["time"]      = MakeNumberParam(
            "optional top-level default time code. Items without their own "
            "\"time\" inherit this; omit at both levels for the default "
            "(static) value.");
        props["layer_id"]  = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as "
            "returned by get_layer_stack. Applies to every entry. Omit to "
            "use the current edit target.");
        props["list_id"]   = MakeStringParam(
            "optional: instead of \"items\", apply to EVERY path in this stored "
            "list (from find_prims store_as / manage_lists). Mutually exclusive "
            "with \"items\". The top-level \"operation\"/\"value\"/\"time\" "
            "defaults are applied to every path — set them.");
        tools.push_back(JsValue(_Tool(
            "set_xforms",
            "QUEUES one or more translate/rotate/scale edits in a single call "
            "using UsdGeomXformCommonAPI — always prefer this over multiple "
            "sequential calls. Top-level \"operation\", \"value\", and \"time\" "
            "act as defaults; each item may override them or supply its own. "
            "To set multiple ops (e.g. translate + rotate) on the same prim, "
            "issue one item per op with the same path. Creates the xform op "
            "if it does not already exist, so this works on freshly-created "
            "prims. Per-item validation errors (no prim, not Xformable, bad "
            "value) are reported in the result and do not block other items. "
            "All items land on the same target layer in a single undoable "
            "command. Re-read with get_attribute_value on xformOp:translate / "
            "xformOp:rotateXYZ / xformOp:scale to confirm. Note: only works on "
            "prims that use the common translate/rotate/scale op layout — not "
            "on matrix-based xforms. Supply either \"items\" or \"list_id\".",
            props, JsArray{})));
    }

    // 12. set_visibilities
    {
        const std::string visEnumNote =
            "one of \"inherited\", \"invisible\", or \"visible\" (USD uses "
            "\"inherited\" not \"visible\" by convention; \"visible\" is "
            "accepted as an alias and normalized to \"inherited\").";

        JsObject itemProps;
        itemProps["path"]       = MakeStringParam(
            "SdfPath of a UsdGeomImageable prim (required per item)");
        itemProps["visibility"] = MakeStringParam(
            "optional: " + visEnumNote
            + " Overrides the top-level \"visibility\". Required if no "
            "top-level default is set.");

        JsObject props;
        props["items"]      = _ObjectArrayParam(itemProps, _Strings({"path"}),
            "list of visibility edits to apply. Each entry requires \"path\"; "
            "\"visibility\" may be set per-item or inherited from the top "
            "level. All entries land on the same target layer in a single "
            "undoable step.");
        props["visibility"] = MakeStringParam(
            "optional top-level default visibility applied to every item "
            "that does not set its own. Use for bulk \"hide all of these\" / "
            "\"show all of these\" edits. " + visEnumNote);
        props["layer_id"]   = MakeStringParam(
            "optional: identifier of the layer to write to, exactly as "
            "returned by get_layer_stack. Applies to every entry. Omit to "
            "use the current edit target.");
        props["list_id"]    = MakeStringParam(
            "optional: instead of \"items\", apply to EVERY path in this stored "
            "list (from find_prims store_as / manage_lists). Mutually exclusive "
            "with \"items\". The top-level \"visibility\" default is applied to "
            "every path — set it (e.g. \"invisible\").");
        tools.push_back(JsValue(_Tool(
            "set_visibilities",
            "QUEUES one or more visibility changes on UsdGeomImageable prims "
            "in a single call — always prefer this over multiple sequential "
            "calls. Convenience wrapper around set_attributes for the common "
            "case. Top-level \"visibility\" acts as a default; each item "
            "may override it. Per-item validation errors (no prim, not "
            "Imageable, invalid token) are reported in the result and do "
            "not block other items. All items land on the same target "
            "layer in a single undoable command. Re-read with "
            "get_value_resolution to confirm and see which layer holds "
            "the new opinion. Supply either \"items\" or \"list_id\".",
            props, JsArray{})));
    }

    // 13. add_references
    {
        JsObject itemProps;
        itemProps["path"]         = MakeStringParam(
            "absolute SdfPath of the prim that will hold the reference arc");
        itemProps["asset_path"]   = MakeStringParam(
            "file path to the referenced USD asset, e.g. \"/assets/hero.usda\". "
            "Pass \"\" for an internal (same-layer) reference.");
        itemProps["prim_path"]    = MakeStringParam(
            "optional: absolute prim path within the referenced asset to target. "
            "Omit to use the asset's defaultPrim.");
        itemProps["layer_offset"] = MakeNumberParam(
            "optional time offset in frames (default 0)");
        itemProps["layer_scale"]  = MakeNumberParam(
            "optional time scale multiplier (default 1)");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps,
            _Strings({"path", "asset_path"}),
            "list of references to add. Each entry has required \"path\" and "
            "\"asset_path\" plus optional \"prim_path\", \"layer_offset\", and "
            "\"layer_scale\". All entries are authored into the same target "
            "layer in a single undoable step.");
        props["layer_id"] = MakeStringParam(
            "optional: identifier of the layer to author in, as returned by "
            "get_layer_stack. Applies to every entry in \"items\". Defaults "
            "to the current edit target.");
        tools.push_back(JsValue(_Tool(
            "add_references",
            "QUEUES adding one or more reference arcs in a single call — "
            "always prefer this over multiple sequential add_reference-style "
            "calls (e.g. when instancing the same asset onto many prims, pass "
            "one entry per target prim). If a target prim has no spec in the "
            "layer a typeless 'over' is created automatically. New arcs are "
            "prepended (strongest). Items are processed in order; per-item "
            "validation errors are reported in the result and do not block "
            "the others. Re-read with get_composition_arcs to confirm.",
            props, _Strings({"items"}))));
    }

    // 14. add_payloads
    {
        JsObject itemProps;
        itemProps["path"]         = MakeStringParam(
            "absolute SdfPath of the prim that will hold the payload arc");
        itemProps["asset_path"]   = MakeStringParam(
            "file path to the payload USD asset, e.g. \"/assets/hero.usda\".");
        itemProps["prim_path"]    = MakeStringParam(
            "optional: absolute prim path within the asset. Omit for defaultPrim.");
        itemProps["layer_offset"] = MakeNumberParam(
            "optional time offset in frames (default 0)");
        itemProps["layer_scale"]  = MakeNumberParam(
            "optional time scale multiplier (default 1)");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps,
            _Strings({"path", "asset_path"}),
            "list of payloads to add. Each entry has required \"path\" and "
            "\"asset_path\" plus optional \"prim_path\", \"layer_offset\", and "
            "\"layer_scale\". All entries are authored into the same target "
            "layer in a single undoable step.");
        props["layer_id"] = MakeStringParam(
            "optional: layer to author in (applies to every entry; defaults "
            "to current edit target)");
        tools.push_back(JsValue(_Tool(
            "add_payloads",
            "QUEUES adding one or more payload arcs in a single call — always "
            "prefer this over multiple sequential add_payload-style calls. "
            "Payloads are like references but are loaded lazily — use them for "
            "heavy geometry or assets that should be unloadable at runtime. If "
            "a target prim has no spec in the layer a typeless 'over' is "
            "created. New arcs are prepended (strongest). Items are processed "
            "in order; per-item validation errors are reported in the result "
            "and do not block the others. Re-read with get_composition_arcs to "
            "confirm.",
            props, _Strings({"items"}))));
    }

    // 15. add_inherits
    {
        JsObject itemProps;
        itemProps["path"]        = MakeStringParam(
            "absolute SdfPath of the prim that will inherit");
        itemProps["target_path"] = MakeStringParam(
            "absolute SdfPath of the class prim to inherit from, "
            "e.g. \"/_class_Hero\"");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps,
            _Strings({"path", "target_path"}),
            "list of inherit arcs to add. Each entry has required \"path\" "
            "(the inheriting prim) and \"target_path\" (the class prim to "
            "inherit from). All entries are authored into the same target "
            "layer in a single undoable step.");
        props["layer_id"] = MakeStringParam(
            "optional: layer to author in (applies to every entry; defaults "
            "to current edit target)");
        tools.push_back(JsValue(_Tool(
            "add_inherits",
            "QUEUES adding one or more inherit arcs in a single call — always "
            "prefer this over multiple sequential add_inherit-style calls. "
            "Inherits are the weakest-but-one composition arc and propagate "
            "opinions to all inheriting prims, making them useful for shared "
            "overrides. If a target prim has no spec in the layer a typeless "
            "'over' is created. New arcs are prepended (strongest). Items are "
            "processed in order; per-item validation errors are reported in "
            "the result and do not block the others. Re-read with "
            "get_composition_arcs to confirm.",
            props, _Strings({"items"}))));
    }

    // 16. add_specializes
    {
        JsObject itemProps;
        itemProps["path"]        = MakeStringParam(
            "absolute SdfPath of the prim that will specialize");
        itemProps["target_path"] = MakeStringParam(
            "absolute SdfPath of the base prim to specialize from");

        JsObject props;
        props["items"]    = _ObjectArrayParam(itemProps,
            _Strings({"path", "target_path"}),
            "list of specialize arcs to add. Each entry has required \"path\" "
            "(the specializing prim) and \"target_path\" (the base prim to "
            "specialize from). All entries are authored into the same target "
            "layer in a single undoable step.");
        props["layer_id"] = MakeStringParam(
            "optional: layer to author in (applies to every entry; defaults "
            "to current edit target)");
        tools.push_back(JsValue(_Tool(
            "add_specializes",
            "QUEUES adding one or more specialize arcs in a single call — "
            "always prefer this over multiple sequential add_specialize-style "
            "calls. Specializes are weaker than inherits — they are the last "
            "arc consulted in LIVRPS. Useful for variant-like overrides where "
            "the base should not retroactively affect the specializing prim. "
            "If a target prim has no spec in the layer a typeless 'over' is "
            "created. New arcs are prepended (strongest). Items are processed "
            "in order; per-item validation errors are reported in the result "
            "and do not block the others. Re-read with get_composition_arcs "
            "to confirm.",
            props, _Strings({"items"}))));
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
            "strongest). The sublayer file does NOT need to exist on disk — the "
            "path is stored regardless. If the user wants the file to actually "
            "exist, first ASK them for permission, then call create_layer_file "
            "before add_sublayer. Returns an error if the path is already a "
            "sublayer of the target. Re-read with get_layer_stack to confirm. "
            "Undoable.",
            props, _Strings({"sublayer_path"}))));
    }

    // create_layer_file
    {
        JsObject props;
        props["path"] = MakeStringParam(
            "path of the USD layer file to create, e.g. \"/shot/anim.usda\" or "
            "\"./anim.usda\". Must end in .usd, .usda or .usdc. A relative path "
            "is resolved against the current stage's root-layer directory — "
            "pass the SAME string you use for add_sublayer's sublayer_path.");
        tools.push_back(JsValue(_Tool(
            "create_layer_file",
            "Creates a new EMPTY USD layer file on disk. Use this to "
            "materialise a sublayer target that does not exist yet, then call "
            "add_sublayer to reference it. This writes a real file to the "
            "user's filesystem: you MUST ask the user for explicit permission "
            "and get a clear yes before calling it. Never clobbers an existing "
            "file (returns an error if the path already exists). Creation is "
            "immediate (not queued), so the result tells you right away whether "
            "it succeeded.",
            props, _Strings({"path"}))));
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

    // open_file
    {
        JsObject props;
        props["path"] = MakeStringParam(
            "absolute or relative filesystem path to the USD file to open "
            "(e.g. \"/shot/asset.usda\", \"~/scenes/hero.usd\")");
        props["mode"] = MakeStringParam(
            "optional: \"stage\" (default) opens the file as a composed USD "
            "stage and makes it the active stage; \"layer\" opens it as an "
            "SDF layer only (useful for inspecting a sublayer without "
            "composing a full stage). Omit to open as stage.");
        tools.push_back(JsValue(_Tool(
            "open_file",
            "Opens a USD file in the editor. With mode=\"stage\" (default) "
            "it loads the file as a composed stage, replacing the currently "
            "active stage. With mode=\"layer\" it opens only the raw SDF "
            "layer. After the open completes, use get_stage_info or "
            "list_children to inspect the newly loaded scene.",
            props, _Strings({"path"}))));
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
