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

// V1 edit set (4 tools), all queued through usdtweak's command system:
//   8.  set_attribute  (scalar simple types only in v1)
//   9.  set_active
//   10. set_variant
//   11. set_visibility
ToolDefs BuildEditToolDefinitions();

// All v1 tools (read-only + edit), in the order above. Use this for the
// production agent; tests can mix and match by calling the two halves.
ToolDefs BuildAllToolDefinitions();

} // namespace UsdAgent
