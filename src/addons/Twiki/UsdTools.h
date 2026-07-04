#pragma once

#include "LLMBackend.h"

namespace UsdAgent {

// Returns the static set of read-only USD inspection tools advertised to the
// model. Build once at startup, reuse forever (the result is value-typed so
// passing it around is cheap; backends never mutate it).
//
// V1 inspection set (7 tools):
//   1. get_prim_info
//   2. get_attribute_value
//   3. get_value_resolution
//   4. get_composition_arcs
//   5. get_layer_stack
//   6. list_children
//   7. find_prims
ToolDefs BuildReadOnlyToolDefinitions();

// Edit set, all queued through usdtweak's command system. Batched plural tools
// take an `items` array or a `list_id` (a stored prim list); singular ones a
// `path`. (Indicative list — see UsdTools.cpp for the full, current set.)
//   set_attributes, set_actives, set_variant, set_visibilities, set_xforms,
//   create_prims, delete_prims, add_references/payloads/inherits/specializes,
//   add_sublayer, set_relationship, select_prims, set_edit_target,
//   run_mutation (UTQL UPDATE/CREATE/DELETE — the write side of run_query)
ToolDefs BuildEditToolDefinitions();

// All v1 tools (read-only + edit), in the order above. Use this for the
// production agent; tests can mix and match by calling the two halves.
ToolDefs BuildAllToolDefinitions();

} // namespace UsdAgent
