#include "UsdToolDispatcher.h"

#include "Commands.h"
#include "JsHelpers.h"
#include "Selection.h"

#include <pxr/base/gf/matrix2d.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerOffset.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/attributeQuery.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/resolveInfo.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/metrics.h>

#include <cstdlib>
#include <exception>
#include <sstream>

namespace UsdAgent {

namespace {

// ----- safe argument helpers ----------------------------------------------

SdfPath _GetPath(const JsObject& args) {
    const std::string s = JsGetString(args, "path");
    if (s.empty() || !SdfPath::IsValidPathString(s)) return SdfPath();
    return SdfPath(s);
}

UsdTimeCode _GetTime(const JsObject& args) {
    if (!JsHasKey(args, "time")) return UsdTimeCode::Default();
    return UsdTimeCode(JsGetDouble(args, "time", 0.0));
}

// ----- per-prim formatting -------------------------------------------------

std::string _SpecifierName(SdfSpecifier s) {
    switch (s) {
        case SdfSpecifierDef:      return "def";
        case SdfSpecifierOver:     return "over";
        case SdfSpecifierClass:    return "class";
        default:                   return "<unknown>";
    }
}

std::string _PrimSummary(const UsdPrim& prim) {
    std::ostringstream oss;
    oss << prim.GetPath().GetString();
    if (prim.GetTypeName().GetString().empty()) {
        oss << " (untyped)";
    } else {
        oss << " (" << prim.GetTypeName().GetString() << ")";
    }
    return oss.str();
}

// Render a layer name suitable for an LLM prompt. Stock identifiers for
// anonymous layers look like "anon:0x12660b060:.usda" — meaningless to a
// user. Collapse to "<anon: tag>" instead, falling back to "<anonymous>"
// when the tag is empty or just a dotted extension.
std::string _LayerName(const SdfLayerHandle& l) {
    if (!l) return "<null layer>";
    if (l->IsAnonymous()) {
        const std::string tag = l->GetDisplayName();
        if (tag.empty() || tag == "." || tag == ".usda" || tag == ".usd"
            || tag == ".usdc" || tag == ".usda.tmp") {
            return "<anonymous>";
        }
        return "<anon:" + tag + ">";
    }
    return l->GetIdentifier();
}

// Find a layer in the stage's layer stack by its identifier or its display
// name as returned by _LayerName (e.g. "<anon:shot.usda>"). Searching by
// display name lets the LLM reuse the strings it saw from get_layer_stack
// without needing to know the raw anonymous-layer identifier.
SdfLayerHandle _FindLayer(const UsdStageRefPtr& stage, const std::string& id) {
    for (const SdfLayerHandle& l : stage->GetLayerStack(/*includeSessionLayers=*/true)) {
        if (l->GetIdentifier() == id || _LayerName(l) == id) return l;
    }
    return {};
}

// Returns the prim spec at `path` in `layer`, creating it (and any missing
// ancestors) as typeless "over" specs when they don't already exist.
// Returns a null handle if any creation step fails.
SdfPrimSpecHandle _EnsurePrimSpec(const SdfLayerHandle& layer, const SdfPath& path) {
    if (SdfPrimSpecHandle existing = layer->GetPrimAtPath(path)) return existing;

    std::vector<SdfPath> missing;
    SdfPath cur = path;
    while (!cur.IsAbsoluteRootPath() && !layer->GetPrimAtPath(cur)) {
        missing.push_back(cur);
        cur = cur.GetParentPath();
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        const SdfPath& p = *it;
        if (p.IsRootPrimPath()) {
            layer->InsertRootPrim(SdfPrimSpec::New(layer, p.GetName(), SdfSpecifierOver));
        } else {
            SdfPrimSpecHandle parent = layer->GetPrimAtPath(p.GetParentPath());
            if (!parent) return {};
            SdfPrimSpec::New(parent, p.GetName(), SdfSpecifierOver);
        }
    }
    return layer->GetPrimAtPath(path);
}

} // namespace

UsdToolDispatcher::UsdToolDispatcher(StageProvider     stageFn,
                                     EditLayerProvider editLayerFn,
                                     SelectionProvider selectionFn)
    : _stageFn(std::move(stageFn))
    , _editLayerFn(std::move(editLayerFn))
    , _selectionFn(std::move(selectionFn)) {}

namespace {

// Final backstop on tool result size. Per-tool caps (kFindPrimsLimit,
// kListChildrenMaxDepth) already bound counts, but a wide stage with long
// prim paths can still push a single result well past what's worth feeding
// the LLM. Truncate at the byte cap and append a marker that tells the
// model exactly what happened so it can refine its next call. Error
// strings ("[error] ...") bypass the cap — they are tiny and would just
// be made less useful by truncation.
std::string _CapResult(std::string result) {
    if (result.size() <= UsdToolDispatcher::kMaxResultBytes) return result;
    if (result.compare(0, 7, "[error]") == 0) return result;

    const size_t original = result.size();
    result.resize(UsdToolDispatcher::kMaxResultBytes);
    result += "\n[... truncated to "
           +  std::to_string(UsdToolDispatcher::kMaxResultBytes)
           +  " of " + std::to_string(original)
           +  " bytes. Refine your call (narrower type/kind/purpose filter, "
              "smaller recursive depth, or a more specific path) to get a "
              "complete result.]";
    return result;
}

} // namespace

std::string UsdToolDispatcher::Dispatch(const std::string& toolName,
                                        const JsObject&    args) {
    std::string result;
    try {
        if      (toolName == "get_stage_info")       result = GetStageInfo(args);
        else if (toolName == "get_prim_info")        result = GetPrimInfo(args);
        else if (toolName == "get_attribute_value")  result = GetAttributeValue(args);
        else if (toolName == "get_value_resolution") result = GetValueResolution(args);
        else if (toolName == "get_composition_arcs") result = GetCompositionArcs(args);
        else if (toolName == "get_layer_stack")      result = GetLayerStack(args);
        else if (toolName == "list_children")        result = ListChildren(args);
        else if (toolName == "find_prims")           result = FindPrims(args);
        else if (toolName == "set_xform")            result = SetXform(args);
        else if (toolName == "set_attribute")        result = SetAttribute(args);
        else if (toolName == "set_active")           result = SetActive(args);
        else if (toolName == "set_variant")          result = SetVariant(args);
        else if (toolName == "set_visibility")       result = SetVisibility(args);
        else if (toolName == "get_selection")        result = GetSelection(args);
        else if (toolName == "select_prims")         result = SelectPrims(args);
        else if (toolName == "create_prim")          result = CreatePrim(args);
        else if (toolName == "add_reference")        result = AddReference(args);
        else if (toolName == "add_payload")          result = AddPayload(args);
        else if (toolName == "add_inherit")          result = AddInherit(args);
        else if (toolName == "add_specialize")       result = AddSpecialize(args);
        else if (toolName == "add_sublayer")         result = AddSublayer(args);
        else if (toolName == "delete_prim")          result = DeletePrim(args);
        else if (toolName == "get_relationship_targets") result = GetRelationshipTargets(args);
        else if (toolName == "set_relationship")     result = SetRelationship(args);
        else                                         result = "[error] unknown tool: " + toolName;
    } catch (const std::exception& e) {
        result = std::string("[error] ") + e.what();
    } catch (...) {
        result = "[error] unknown exception in tool " + toolName;
    }
    return _CapResult(std::move(result));
}

// --------------------------------------------------------------------------
// 0. get_stage_info
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetStageInfo(const JsObject& /*args*/) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    std::ostringstream oss;

    UsdPrim defPrim = stage->GetDefaultPrim();
    oss << "defaultPrim: "
        << (defPrim ? defPrim.GetPath().GetString() : "<none>") << "\n";

    oss << "startTimeCode: "      << stage->GetStartTimeCode()     << "\n";
    oss << "endTimeCode: "        << stage->GetEndTimeCode()        << "\n";
    oss << "timeCodesPerSecond: " << stage->GetTimeCodesPerSecond() << "\n";
    oss << "upAxis: "             << UsdGeomGetStageUpAxis(stage).GetString() << "\n";
    oss << "metersPerUnit: "      << UsdGeomGetStageMetersPerUnit(stage)      << "\n";

    size_t layerCount = stage->GetLayerStack(/*includeSessionLayers=*/true).size();
    oss << "layerCount: " << layerCount << "\n";

    size_t primCount = 0;
    for (const UsdPrim& p : stage->Traverse()) { (void)p; ++primCount; }
    oss << "primCount: " << primCount << "\n";

    return oss.str();
}

// --------------------------------------------------------------------------
// 1. get_prim_info
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetPrimInfo(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";

    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    std::ostringstream oss;
    oss << "path: "        << prim.GetPath().GetString() << "\n";
    oss << "type: "        << (prim.GetTypeName().GetString().empty()
                                ? "<untyped>"
                                : prim.GetTypeName().GetString()) << "\n";
    oss << "specifier: "   << _SpecifierName(prim.GetSpecifier()) << "\n";
    oss << "active: "      << (prim.IsActive() ? "true" : "false") << "\n";
    oss << "instanceable: "<< (prim.IsInstanceable() ? "true" : "false") << "\n";

    TfToken kind;
    if (UsdModelAPI(prim).GetKind(&kind)) {
        oss << "kind: " << kind.GetString() << "\n";
    } else {
        oss << "kind: <none>\n";
    }

    UsdGeomImageable img(prim);
    if (img) {
        TfToken purpose;
        if (img.GetPurposeAttr().Get(&purpose)) {
            oss << "purpose: " << purpose.GetString() << "\n";
        }
        TfToken vis;
        if (img.GetVisibilityAttr().Get(&vis)) {
            oss << "visibility: " << vis.GetString() << "\n";
        }
    }

    // Brief children + property listings.
    auto children = prim.GetChildren();
    int childCount = 0;
    for (auto _ : children) { (void)_; ++childCount; }
    oss << "child_count: " << childCount;
    if (childCount > 0) {
        oss << " (";
        int i = 0;
        for (const UsdPrim& c : prim.GetChildren()) {
            if (i++) oss << ", ";
            oss << c.GetName().GetString();
            if (i >= 8) { oss << ", ..."; break; }
        }
        oss << ")";
    }
    oss << "\n";

    auto props = prim.GetPropertyNames();
    oss << "property_count: " << props.size();
    if (!props.empty()) {
        oss << " (";
        for (size_t i = 0; i < props.size() && i < 8; ++i) {
            if (i) oss << ", ";
            oss << props[i].GetString();
        }
        if (props.size() > 8) oss << ", ...";
        oss << ")";
    }
    oss << "\n";

    return oss.str();
}

// --------------------------------------------------------------------------
// 2. get_attribute_value
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetAttributeValue(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    const std::string attrName = JsGetString(args, "attribute");
    if (attrName.empty()) return "[error] missing 'attribute' argument";

    UsdAttribute attr = prim.GetAttribute(TfToken(attrName));
    if (!attr) return "[error] no attribute '" + attrName + "' on " + path.GetString();

    UsdTimeCode time = _GetTime(args);

    VtValue value;
    if (!attr.Get(&value, time)) {
        std::ostringstream oss;
        oss << "no value for " << path.GetString() << "." << attrName;
        oss << " at time " << (time.IsDefault() ? "DEFAULT"
                               : TfStringPrintf("%g", time.GetValue()));
        return oss.str();
    }

    std::ostringstream oss;
    oss << path.GetString() << "." << attrName;
    oss << " (" << attr.GetTypeName().GetAsToken().GetString() << ")";
    oss << " @ " << (time.IsDefault() ? std::string("DEFAULT")
                     : TfStringPrintf("%g", time.GetValue()));
    oss << " = " << TfStringify(value);
    return oss.str();
}

// --------------------------------------------------------------------------
// 3. get_value_resolution
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetValueResolution(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    const std::string attrName = JsGetString(args, "attribute");
    if (attrName.empty()) return "[error] missing 'attribute' argument";

    UsdAttribute attr = prim.GetAttribute(TfToken(attrName));
    if (!attr) return "[error] no attribute '" + attrName + "' on " + path.GetString();

    std::ostringstream oss;
    oss << "attribute: " << path.GetString() << "." << attrName << "\n";

    // Resolved value at default time.
    VtValue resolved;
    if (attr.Get(&resolved, UsdTimeCode::Default())) {
        oss << "resolved (DEFAULT): " << TfStringify(resolved) << "\n";
    } else {
        oss << "resolved (DEFAULT): <no opinion>\n";
    }

    // Property stack — strongest first.
    auto stack = attr.GetPropertyStack(UsdTimeCode::Default());
    oss << "opinions (" << stack.size() << "):\n";
    for (size_t i = 0; i < stack.size(); ++i) {
        const SdfPropertySpecHandle& spec = stack[i];
        SdfLayerHandle layer = spec->GetLayer();
        VtValue v;
        spec->HasField(SdfFieldKeys->Default)
            ? (v = spec->GetField(SdfFieldKeys->Default), 0)
            : 0;
        oss << "  " << (i == 0 ? "[winning] " : "          ");
        oss << _LayerName(layer);
        if (!v.IsEmpty()) oss << " = " << TfStringify(v);
        oss << "\n";
    }

    if (stack.empty()) {
        oss << "  (no opinions on any layer)\n";
    } else {
        oss << "Composition order checked: "
               "local > references > inherits > specializes > variants > payload";
    }
    return oss.str();
}

// --------------------------------------------------------------------------
// 4. get_composition_arcs
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetCompositionArcs(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    UsdPrimCompositionQuery query(prim);
    auto arcs = query.GetCompositionArcs();

    std::ostringstream oss;
    oss << "composition arcs on " << path.GetString() << " (" << arcs.size() << "):\n";

    for (const UsdPrimCompositionQueryArc& arc : arcs) {
        oss << "  - ";
        switch (arc.GetArcType()) {
            case PcpArcTypeRoot:        oss << "root";        break;
            case PcpArcTypeReference:   oss << "reference";   break;
            case PcpArcTypePayload:     oss << "payload";     break;
            case PcpArcTypeInherit:     oss << "inherit";     break;
            case PcpArcTypeSpecialize:  oss << "specialize";  break;
            case PcpArcTypeVariant:     oss << "variant";     break;
            default:                    oss << "unknown";     break;
        }
        SdfLayerHandle layer = arc.GetIntroducingLayer();
        if (layer) {
            oss << " introduced by " << _LayerName(layer);
        }
        SdfPath targetPath = arc.GetTargetPrimPath();
        if (!targetPath.IsEmpty()) {
            oss << " → " << targetPath.GetString();
        }
        oss << (arc.IsImplicit() ? " (implicit)" : "");
        oss << "\n";
    }

    // Also list variant sets / selections for convenience.
    UsdVariantSets vsets = prim.GetVariantSets();
    auto names = vsets.GetNames();
    if (!names.empty()) {
        oss << "variant sets:\n";
        for (const std::string& n : names) {
            oss << "  - " << n << " = "
                << vsets.GetVariantSelection(n) << "\n";
        }
    }
    return oss.str();
}

// --------------------------------------------------------------------------
// 5. get_layer_stack
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetLayerStack(const JsObject& /*args*/) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfLayerHandleVector layers = stage->GetLayerStack(/*includeSessionLayers=*/true);
    std::ostringstream oss;
    oss << "layer stack (strongest first, " << layers.size() << " layers):\n";
    for (size_t i = 0; i < layers.size(); ++i) {
        const SdfLayerHandle& l = layers[i];
        oss << "  " << (i + 1) << ". " << _LayerName(l);
        if (l == stage->GetEditTarget().GetLayer()) oss << "  [edit target]";
        if (l == stage->GetSessionLayer())          oss << "  [session]";
        if (l == stage->GetRootLayer())             oss << "  [root]";
        if (stage->IsLayerMuted(l->GetIdentifier())) oss << "  [muted]";
        if (!l->PermissionToEdit())                 oss << "  [readonly]";
        oss << "\n";
    }
    return oss.str();
}

// --------------------------------------------------------------------------
// 6. list_children
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::ListChildren(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    UsdPrim prim;
    if (path.IsEmpty()) {
        prim = stage->GetPseudoRoot();
    } else {
        prim = stage->GetPrimAtPath(path);
        if (!prim) return "[error] no prim at " + path.GetString();
    }

    const bool recursive = JsGetBool(args, "recursive", false);

    std::ostringstream oss;
    oss << "children of " << prim.GetPath().GetString();
    oss << (recursive ? " (recursive, max depth "
                        + std::to_string(kListChildrenMaxDepth) + "):\n"
                      : ":\n");

    if (!recursive) {
        int n = 0;
        for (const UsdPrim& c : prim.GetChildren()) {
            oss << "  " << _PrimSummary(c) << "\n";
            ++n;
        }
        if (n == 0) oss << "  (no children)\n";
        return oss.str();
    }

    // Recursive walk with depth cap.
    int truncated = 0;
    int written   = 0;
    for (UsdPrim p : UsdPrimRange(prim)) {
        if (p == prim) continue;
        int depth = p.GetPath().GetPathElementCount()
                  - prim.GetPath().GetPathElementCount();
        if (depth > kListChildrenMaxDepth) { ++truncated; continue; }
        oss << "  " << std::string(depth * 2, ' ')
            << _PrimSummary(p) << "\n";
        ++written;
    }
    if (written == 0)   oss << "  (no descendants)\n";
    if (truncated > 0)  oss << "  [...truncated " << truncated
                            << " deeper than depth "
                            << kListChildrenMaxDepth << "]\n";
    return oss.str();
}

// --------------------------------------------------------------------------
// 7. find_prims
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::FindPrims(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    const std::string typeFilter    = JsGetString(args, "type");
    const std::string kindFilter    = JsGetString(args, "kind");
    const std::string purposeFilter = JsGetString(args, "purpose");
    const bool        hasActive     = JsHasKey(args, "active");
    const bool        activeFilter  = JsGetBool(args, "active", true);

    std::ostringstream oss;
    oss << "find_prims: ";
    if (!typeFilter.empty())    oss << "type=" << typeFilter << " ";
    if (!kindFilter.empty())    oss << "kind=" << kindFilter << " ";
    if (!purposeFilter.empty()) oss << "purpose=" << purposeFilter << " ";
    if (hasActive)              oss << "active=" << (activeFilter ? "true" : "false") << " ";
    if (typeFilter.empty() && kindFilter.empty() && purposeFilter.empty() && !hasActive) {
        oss << "(no filters — listing all prims)";
    }
    oss << "\n";

    size_t kept     = 0;
    size_t scanned  = 0;
    bool   truncated = false;
    for (const UsdPrim& prim : stage->Traverse()) {
        ++scanned;
        if (!typeFilter.empty() &&
            prim.GetTypeName().GetString() != typeFilter) continue;
        if (!kindFilter.empty()) {
            TfToken k;
            if (!UsdModelAPI(prim).GetKind(&k) || k.GetString() != kindFilter)
                continue;
        }
        if (!purposeFilter.empty()) {
            UsdGeomImageable img(prim);
            TfToken p;
            if (!img || !img.GetPurposeAttr().Get(&p) ||
                p.GetString() != purposeFilter) continue;
        }
        if (hasActive && prim.IsActive() != activeFilter) continue;

        if (kept >= kFindPrimsLimit) { truncated = true; continue; }

        oss << "  - " << _PrimSummary(prim) << "\n";
        ++kept;
    }
    oss << "matched " << kept << " of " << scanned << " prims";
    if (truncated) {
        oss << " (truncated to first " << kFindPrimsLimit << ")";
    }
    oss << "\n";
    return oss.str();
}

// --------------------------------------------------------------------------
// set_xform helper — parse a JsValue as a double (int or real accepted)
// --------------------------------------------------------------------------
namespace {

bool _ParseVec3(const JsArray& arr, double& x, double& y, double& z,
                std::string& err) {
    if (arr.size() != 3) {
        err = "'value' must be an array of exactly 3 numbers, got "
            + std::to_string(arr.size());
        return false;
    }
    auto toDouble = [](const JsValue& v, double& out) -> bool {
        if (v.IsReal())  { out = v.GetReal();                   return true; }
        if (v.IsInt())   { out = static_cast<double>(v.GetInt64()); return true; }
        return false;
    };
    if (!toDouble(arr[0], x) || !toDouble(arr[1], y) || !toDouble(arr[2], z)) {
        err = "each element of 'value' must be a number";
        return false;
    }
    return true;
}

} // namespace

// --------------------------------------------------------------------------
// Edit-tool helpers
// --------------------------------------------------------------------------
namespace {

// Strip brackets/parens/commas and extract all floating-point numbers.
void _ExtractNums(const std::string& text, std::vector<double>& out) {
    std::string s = text;
    for (char& c : s)
        if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']') c = ' ';
    std::istringstream iss(s);
    double v;
    while (iss >> v) out.push_back(v);
}

// Expect exactly n numbers; fills errOut and returns false on mismatch.
bool _ExpectN(const std::string& text, size_t n,
              std::vector<double>& v, std::string* errOut) {
    _ExtractNums(text, v);
    if (v.size() != n) {
        *errOut = "expected " + std::to_string(n) + " number"
                + (n == 1 ? "" : "s") + ", got "
                + std::to_string(v.size()) + " in \"" + text + "\"";
        return false;
    }
    return true;
}

// Expect a count that is a multiple of stride (for array types).
bool _ExpectMultiple(const std::string& text, size_t stride,
                     std::vector<double>& v, std::string* errOut) {
    _ExtractNums(text, v);
    if (v.size() % stride != 0) {
        *errOut = "element count (" + std::to_string(v.size())
                + ") is not a multiple of " + std::to_string(stride);
        return false;
    }
    return true;
}

// Tokenize a string/token/asset array value: "[a, b, c]" → {"a","b","c"}.
// Handles optional outer brackets and single/double quotes.
std::vector<std::string> _SplitStringTokens(const std::string& text) {
    std::string s = text;
    if (!s.empty() && s.front() == '[') s = s.substr(1);
    if (!s.empty() && s.back()  == ']') s.pop_back();
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        size_t a = tok.find_first_not_of(" \t\r\n\"'");
        size_t b = tok.find_last_not_of(" \t\r\n\"'");
        if (a != std::string::npos)
            result.push_back(tok.substr(a, b - a + 1));
    }
    return result;
}

// Coerce a string value into a VtValue matching the attribute's declared type.
//
// Scalar types: pass the value directly ("3.14", "true", "myToken").
// Vector/matrix types: space- or comma-separated numbers in optional
//   brackets/parens — e.g. "1 2 3", "(0.5, 0.5, 0.5)", "[1,0,0,0,…]".
// Quaternions: "w x y z" (real part first).
// Scalar arrays: space- or comma-separated numbers — e.g. "1 2 3 4".
// String/token/asset arrays: comma-separated, optional quotes — "a, b, c".
// Vec-N arrays: flat, e.g. "0.1 0.2 0.3 0.4 0.5 0.6" for two float3 elements.
//
// Returns true on success and writes to *out. On failure writes the reason
// into *errOut.
bool _ParseValueForAttribute(const std::string& text,
                             const SdfValueTypeName& type,
                             VtValue* out,
                             std::string* errOut) {
    if (!type) { *errOut = "attribute has no declared type"; return false; }
    const std::string t = type.GetAsToken().GetString();

    // ---- scalars -----------------------------------------------------------
    if (t == "bool") {
        if (text=="true"||text=="1"||text=="True")  { *out=VtValue(true);  return true; }
        if (text=="false"||text=="0"||text=="False"){ *out=VtValue(false); return true; }
        *errOut = "expected bool ('true'/'false'), got '" + text + "'"; return false;
    }
    if (t == "int" || t == "int32") {
        try { *out = VtValue(int(std::stoi(text))); return true; }
        catch (...) { *errOut = "expected int, got '" + text + "'"; return false; }
    }
    if (t == "int64") {
        try { *out = VtValue(int64_t(std::stoll(text))); return true; }
        catch (...) { *errOut = "expected int64, got '" + text + "'"; return false; }
    }
    if (t == "uint" || t == "uint32") {
        try { *out = VtValue(uint32_t(std::stoul(text))); return true; }
        catch (...) { *errOut = "expected uint, got '" + text + "'"; return false; }
    }
    if (t == "uint64") {
        try { *out = VtValue(uint64_t(std::stoull(text))); return true; }
        catch (...) { *errOut = "expected uint64, got '" + text + "'"; return false; }
    }
    if (t == "float") {
        try { *out = VtValue(float(std::stof(text))); return true; }
        catch (...) { *errOut = "expected float, got '" + text + "'"; return false; }
    }
    if (t == "double" || t == "timecode") {
        try { *out = VtValue(double(std::stod(text))); return true; }
        catch (...) { *errOut = "expected double, got '" + text + "'"; return false; }
    }
    if (t == "half") {
        try { *out = VtValue(GfHalf(std::stof(text))); return true; }
        catch (...) { *errOut = "expected half, got '" + text + "'"; return false; }
    }
    if (t == "string") { *out = VtValue(text); return true; }
    if (t == "token")  { *out = VtValue(TfToken(text)); return true; }
    if (t == "asset")  { *out = VtValue(SdfAssetPath(text)); return true; }

    // ---- Vec2 --------------------------------------------------------------
    if (t == "float2") {
        std::vector<double> v; if (!_ExpectN(text, 2, v, errOut)) return false;
        *out = VtValue(GfVec2f(float(v[0]), float(v[1]))); return true;
    }
    if (t == "double2") {
        std::vector<double> v; if (!_ExpectN(text, 2, v, errOut)) return false;
        *out = VtValue(GfVec2d(v[0], v[1])); return true;
    }
    if (t == "half2") {
        std::vector<double> v; if (!_ExpectN(text, 2, v, errOut)) return false;
        *out = VtValue(GfVec2h(GfHalf(v[0]), GfHalf(v[1]))); return true;
    }
    if (t == "int2") {
        std::vector<double> v; if (!_ExpectN(text, 2, v, errOut)) return false;
        *out = VtValue(GfVec2i(int(v[0]), int(v[1]))); return true;
    }

    // ---- Vec3 + role aliases -----------------------------------------------
    if (t=="float3"||t=="color3f"||t=="point3f"||t=="normal3f"||t=="vector3f") {
        std::vector<double> v; if (!_ExpectN(text, 3, v, errOut)) return false;
        *out = VtValue(GfVec3f(float(v[0]), float(v[1]), float(v[2]))); return true;
    }
    if (t=="double3"||t=="color3d"||t=="point3d"||t=="normal3d"||t=="vector3d") {
        std::vector<double> v; if (!_ExpectN(text, 3, v, errOut)) return false;
        *out = VtValue(GfVec3d(v[0], v[1], v[2])); return true;
    }
    if (t=="half3"||t=="color3h"||t=="point3h"||t=="normal3h"||t=="vector3h") {
        std::vector<double> v; if (!_ExpectN(text, 3, v, errOut)) return false;
        *out = VtValue(GfVec3h(GfHalf(v[0]), GfHalf(v[1]), GfHalf(v[2]))); return true;
    }
    if (t == "int3") {
        std::vector<double> v; if (!_ExpectN(text, 3, v, errOut)) return false;
        *out = VtValue(GfVec3i(int(v[0]), int(v[1]), int(v[2]))); return true;
    }

    // ---- Vec4 + role aliases -----------------------------------------------
    if (t=="float4"||t=="color4f") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfVec4f(float(v[0]),float(v[1]),float(v[2]),float(v[3]))); return true;
    }
    if (t=="double4"||t=="color4d") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfVec4d(v[0], v[1], v[2], v[3])); return true;
    }
    if (t=="half4"||t=="color4h") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfVec4h(GfHalf(v[0]),GfHalf(v[1]),GfHalf(v[2]),GfHalf(v[3]))); return true;
    }
    if (t == "int4") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfVec4i(int(v[0]),int(v[1]),int(v[2]),int(v[3]))); return true;
    }

    // ---- Matrices (row-major) ----------------------------------------------
    if (t == "matrix2d") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        GfMatrix2d m; m[0][0]=v[0]; m[0][1]=v[1]; m[1][0]=v[2]; m[1][1]=v[3];
        *out = VtValue(m); return true;
    }
    if (t == "matrix3d") {
        std::vector<double> v; if (!_ExpectN(text, 9, v, errOut)) return false;
        GfMatrix3d m;
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) m[i][j]=v[i*3+j];
        *out = VtValue(m); return true;
    }
    if (t == "matrix4d" || t == "frame4d") {
        std::vector<double> v; if (!_ExpectN(text, 16, v, errOut)) return false;
        GfMatrix4d m;
        for (int i=0;i<4;++i) for (int j=0;j<4;++j) m[i][j]=v[i*4+j];
        *out = VtValue(m); return true;
    }

    // ---- Quaternions ("w x y z") -------------------------------------------
    if (t == "quatf") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfQuatf(float(v[0]),float(v[1]),float(v[2]),float(v[3]))); return true;
    }
    if (t == "quatd") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfQuatd(v[0], v[1], v[2], v[3])); return true;
    }
    if (t == "quath") {
        std::vector<double> v; if (!_ExpectN(text, 4, v, errOut)) return false;
        *out = VtValue(GfQuath(GfHalf(v[0]),GfHalf(v[1]),GfHalf(v[2]),GfHalf(v[3]))); return true;
    }

    // ---- Scalar arrays -----------------------------------------------------
    if (t == "bool[]") {
        auto toks = _SplitStringTokens(text);
        VtArray<bool> arr(toks.size());
        for (size_t i=0;i<toks.size();++i) {
            const std::string& s = toks[i];
            if (s=="true"||s=="1"||s=="True")   arr[i]=true;
            else if (s=="false"||s=="0"||s=="False") arr[i]=false;
            else { *errOut="invalid bool '"+s+"' in array"; return false; }
        }
        *out = VtValue(arr); return true;
    }
    if (t=="int[]"||t=="int32[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<int> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=int(v[i]);
        *out = VtValue(arr); return true;
    }
    if (t == "int64[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<int64_t> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=int64_t(v[i]);
        *out = VtValue(arr); return true;
    }
    if (t=="uint[]"||t=="uint32[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<uint32_t> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=uint32_t(v[i]);
        *out = VtValue(arr); return true;
    }
    if (t == "uint64[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<uint64_t> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=uint64_t(v[i]);
        *out = VtValue(arr); return true;
    }
    if (t == "float[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<float> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=float(v[i]);
        *out = VtValue(arr); return true;
    }
    if (t=="double[]"||t=="timecode[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<double> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=v[i];
        *out = VtValue(arr); return true;
    }
    if (t == "half[]") {
        std::vector<double> v; _ExtractNums(text, v);
        VtArray<GfHalf> arr(v.size());
        for (size_t i=0;i<v.size();++i) arr[i]=GfHalf(v[i]);
        *out = VtValue(arr); return true;
    }

    // ---- String-like arrays ------------------------------------------------
    if (t == "string[]") {
        auto toks = _SplitStringTokens(text);
        VtArray<std::string> arr(toks.size());
        for (size_t i=0;i<toks.size();++i) arr[i]=toks[i];
        *out = VtValue(arr); return true;
    }
    if (t == "token[]") {
        auto toks = _SplitStringTokens(text);
        VtArray<TfToken> arr(toks.size());
        for (size_t i=0;i<toks.size();++i) arr[i]=TfToken(toks[i]);
        *out = VtValue(arr); return true;
    }
    if (t == "asset[]") {
        auto toks = _SplitStringTokens(text);
        VtArray<SdfAssetPath> arr(toks.size());
        for (size_t i=0;i<toks.size();++i) arr[i]=SdfAssetPath(toks[i]);
        *out = VtValue(arr); return true;
    }

    // ---- Vec2 arrays -------------------------------------------------------
    if (t == "float2[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 2, v, errOut)) return false;
        VtArray<GfVec2f> arr(v.size()/2);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec2f(float(v[i*2]),float(v[i*2+1]));
        *out = VtValue(arr); return true;
    }
    if (t == "double2[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 2, v, errOut)) return false;
        VtArray<GfVec2d> arr(v.size()/2);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec2d(v[i*2], v[i*2+1]);
        *out = VtValue(arr); return true;
    }
    if (t == "half2[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 2, v, errOut)) return false;
        VtArray<GfVec2h> arr(v.size()/2);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec2h(GfHalf(v[i*2]),GfHalf(v[i*2+1]));
        *out = VtValue(arr); return true;
    }
    if (t == "int2[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 2, v, errOut)) return false;
        VtArray<GfVec2i> arr(v.size()/2);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec2i(int(v[i*2]),int(v[i*2+1]));
        *out = VtValue(arr); return true;
    }

    // ---- Vec3 arrays + role aliases ----------------------------------------
    if (t=="float3[]"||t=="color3f[]"||t=="point3f[]"||t=="normal3f[]"||t=="vector3f[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 3, v, errOut)) return false;
        VtArray<GfVec3f> arr(v.size()/3);
        for (size_t i=0;i<arr.size();++i)
            arr[i]=GfVec3f(float(v[i*3]),float(v[i*3+1]),float(v[i*3+2]));
        *out = VtValue(arr); return true;
    }
    if (t=="double3[]"||t=="color3d[]"||t=="point3d[]"||t=="normal3d[]"||t=="vector3d[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 3, v, errOut)) return false;
        VtArray<GfVec3d> arr(v.size()/3);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec3d(v[i*3],v[i*3+1],v[i*3+2]);
        *out = VtValue(arr); return true;
    }
    if (t=="half3[]"||t=="color3h[]"||t=="point3h[]"||t=="normal3h[]"||t=="vector3h[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 3, v, errOut)) return false;
        VtArray<GfVec3h> arr(v.size()/3);
        for (size_t i=0;i<arr.size();++i)
            arr[i]=GfVec3h(GfHalf(v[i*3]),GfHalf(v[i*3+1]),GfHalf(v[i*3+2]));
        *out = VtValue(arr); return true;
    }
    if (t == "int3[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 3, v, errOut)) return false;
        VtArray<GfVec3i> arr(v.size()/3);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec3i(int(v[i*3]),int(v[i*3+1]),int(v[i*3+2]));
        *out = VtValue(arr); return true;
    }

    // ---- Vec4 arrays + role aliases ----------------------------------------
    if (t=="float4[]"||t=="color4f[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 4, v, errOut)) return false;
        VtArray<GfVec4f> arr(v.size()/4);
        for (size_t i=0;i<arr.size();++i)
            arr[i]=GfVec4f(float(v[i*4]),float(v[i*4+1]),float(v[i*4+2]),float(v[i*4+3]));
        *out = VtValue(arr); return true;
    }
    if (t=="double4[]"||t=="color4d[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 4, v, errOut)) return false;
        VtArray<GfVec4d> arr(v.size()/4);
        for (size_t i=0;i<arr.size();++i) arr[i]=GfVec4d(v[i*4],v[i*4+1],v[i*4+2],v[i*4+3]);
        *out = VtValue(arr); return true;
    }
    if (t=="half4[]"||t=="color4h[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 4, v, errOut)) return false;
        VtArray<GfVec4h> arr(v.size()/4);
        for (size_t i=0;i<arr.size();++i)
            arr[i]=GfVec4h(GfHalf(v[i*4]),GfHalf(v[i*4+1]),GfHalf(v[i*4+2]),GfHalf(v[i*4+3]));
        *out = VtValue(arr); return true;
    }
    if (t == "int4[]") {
        std::vector<double> v; if (!_ExpectMultiple(text, 4, v, errOut)) return false;
        VtArray<GfVec4i> arr(v.size()/4);
        for (size_t i=0;i<arr.size();++i)
            arr[i]=GfVec4i(int(v[i*4]),int(v[i*4+1]),int(v[i*4+2]),int(v[i*4+3]));
        *out = VtValue(arr); return true;
    }

    *errOut = "type '" + t + "' is not yet supported by set_attribute. "
              "Supported: scalars (bool/int/int64/uint/uint64/float/double/half/timecode/"
              "string/token/asset); vec2/3/4 (f/d/h/i); color3/4(f/d/h); "
              "point3/normal3/vector3(f/d/h); matrix2/3/4d; frame4d; quatf/quatd/quath; "
              "scalar arrays of those types; string[]/token[]/asset[]; "
              "vec2/3/4 arrays of those types.";
    return false;
}

} // namespace

// --------------------------------------------------------------------------
// set_xform  (queued via UsdGeomXformCommonAPI)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetXform(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    if (!UsdGeomXformable(prim)) {
        return "[error] " + path.GetString()
             + " is not UsdGeomXformable — cannot set xform ops";
    }

    const std::string op = JsGetString(args, "operation");
    if (op != "translate" && op != "rotate" && op != "scale") {
        return "[error] 'operation' must be \"translate\", \"rotate\", or "
               "\"scale\"; got \"" + op + "\"";
    }

    if (!JsHasKey(args, "value")) return "[error] missing 'value' argument";
    JsArray arr = JsGetArray(args, "value");
    double x = 0, y = 0, z = 0;
    std::string parseErr;
    if (!_ParseVec3(arr, x, y, z, parseErr)) return "[error] " + parseErr;

    UsdTimeCode time = _GetTime(args);
    const std::string timeStr = time.IsDefault()
        ? std::string("DEFAULT")
        : TfStringPrintf("%g", time.GetValue());

    SdfLayerHandle editLayer = stage->GetEditTarget().GetLayer();
    UsdStageRefPtr stageCopy = stage;

    std::function<void()> fn;
    if (op == "translate") {
        GfVec3d t(x, y, z);
        fn = [stageCopy, path, t, time]() {
            UsdGeomXformCommonAPI api(stageCopy->GetPrimAtPath(path));
            api.SetTranslate(t, time);
        };
    } else if (op == "rotate") {
        GfVec3f r(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        fn = [stageCopy, path, r, time]() {
            UsdGeomXformCommonAPI api(stageCopy->GetPrimAtPath(path));
            api.SetRotate(r, UsdGeomXformCommonAPI::RotationOrderXYZ, time);
        };
    } else {  // scale
        GfVec3f s(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        fn = [stageCopy, path, s, time]() {
            UsdGeomXformCommonAPI api(stageCopy->GetPrimAtPath(path));
            api.SetScale(s, time);
        };
    }
    ExecuteAfterDraw<UsdFunctionCall>(editLayer, fn);

    std::ostringstream oss;
    oss << "Queued: " << op << " on " << path.GetString()
        << " = [" << x << ", " << y << ", " << z << "]"
        << " @ " << timeStr << ". Re-read on next step to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 8. set_attribute  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetAttribute(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    const std::string attrName = JsGetString(args, "attribute");
    if (attrName.empty()) return "[error] missing 'attribute' argument";

    UsdAttribute attr = prim.GetAttribute(TfToken(attrName));
    if (!attr) return "[error] no attribute '" + attrName + "' on " + path.GetString();

    const std::string valueText = JsGetString(args, "value");
    if (valueText.empty() && !JsHasKey(args, "value")) {
        return "[error] missing 'value' argument";
    }

    VtValue value;
    std::string err;
    if (!_ParseValueForAttribute(valueText, attr.GetTypeName(), &value, &err)) {
        return "[error] " + err;
    }

    UsdTimeCode time = _GetTime(args);
    const std::string timeStr = time.IsDefault()
        ? std::string("DEFAULT")
        : TfStringPrintf("%g", time.GetValue());

    // Resolve the target layer — either named via layer_id or the edit target.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    std::string targetLayerName;

    if (layerIdArg.empty()) {
        // Default: write to the current edit target.
        ExecuteAfterDraw<AttributeSet>(attr, value, time);
        SdfLayerHandle editLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = editLayer ? _LayerName(editLayer) : "<none>";
    } else {
        // Explicit layer: bypass the edit target.
        SdfLayerHandle layer = _FindLayer(stage, layerIdArg);
        if (!layer) {
            return "[error] layer '" + layerIdArg + "' not found in the stage's "
                   "layer stack. Call get_layer_stack to see available layers.";
        }
        if (!layer->PermissionToEdit()) {
            return "[error] layer '" + layerIdArg + "' is read-only";
        }
        targetLayerName = _LayerName(layer);

        // Capture by value so the lambda is safe after this frame.
        SdfPath attrPath = attr.GetPath();
        UsdStageRefPtr stageCopy = stage;
        std::string ident = layer->GetIdentifier();
        std::function<void()> fn = [stageCopy, attrPath, value, time, ident]() {
            SdfLayerHandle l = SdfLayer::Find(ident);
            if (!l) return;
            UsdEditContext ctx(stageCopy, UsdEditTarget(l));
            UsdAttribute a = stageCopy->GetAttributeAtPath(attrPath);
            if (a) a.Set(value, time);
        };
        ExecuteAfterDraw<UsdFunctionCall>(layer, fn);
    }

    std::ostringstream oss;
    oss << "Queued: set " << path.GetString() << "." << attrName
        << " (" << attr.GetTypeName().GetAsToken().GetString() << ") = "
        << valueText << " @ " << timeStr
        << " on layer " << targetLayerName
        << ". Re-read on next step to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 9. set_active  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetActive(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    if (!JsHasKey(args, "active")) {
        return "[error] missing 'active' argument (bool)";
    }
    const bool active = JsGetBool(args, "active");

    ExecuteAfterDraw(&UsdPrim::SetActive, prim, active);

    std::ostringstream oss;
    oss << "Queued: SetActive(" << (active ? "true" : "false") << ") on "
        << path.GetString() << ". Re-read on next step to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 10. set_variant  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetVariant(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    const std::string vsetName = JsGetString(args, "variantSet");
    if (vsetName.empty()) return "[error] missing 'variantSet' argument";

    if (!prim.HasVariantSets()) {
        return "[error] " + path.GetString() + " has no variant sets";
    }
    UsdVariantSet vset = prim.GetVariantSet(vsetName);
    if (!vset) {
        return "[error] no variant set '" + vsetName + "' on " + path.GetString();
    }

    const std::string variantName = JsGetString(args, "variant");
    // Empty variant means "clear selection" — that's a valid request.

    ExecuteAfterDraw(&UsdVariantSet::SetVariantSelection, vset, variantName);

    std::ostringstream oss;
    oss << "Queued: SetVariantSelection(" << vsetName << "='"
        << variantName << "') on " << path.GetString()
        << ". Re-read on next step to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 11. set_visibility  (queued — convenience wrapper around set_attribute)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetVisibility(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    const std::string vis = JsGetString(args, "visibility");
    if (vis != "inherited" && vis != "invisible" && vis != "visible") {
        return "[error] 'visibility' must be \"inherited\", \"invisible\", "
               "or \"visible\"; got \"" + vis + "\"";
    }

    UsdGeomImageable img(prim);
    if (!img) {
        return "[error] " + path.GetString()
             + " is not UsdGeomImageable; visibility cannot be set";
    }
    UsdAttribute attr = img.GetVisibilityAttr();
    if (!attr) {
        return "[error] no visibility attribute on " + path.GetString();
    }

    ExecuteAfterDraw<AttributeSet>(attr, VtValue(TfToken(vis)),
                                   UsdTimeCode::Default());

    std::ostringstream oss;
    oss << "Queued: set visibility on " << path.GetString() << " = '"
        << vis << "'. Re-read on next step to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 12. get_selection
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetSelection(const JsObject& args) const {
    if (!_selectionFn) return "[error] selection provider not configured";
    Selection* sel = _selectionFn();
    if (!sel) return "[error] no active selection";

    const std::string scopeFilter = JsGetString(args, "scope");
    const bool wantStage = scopeFilter.empty() || scopeFilter == "stage";
    const bool wantLayer = scopeFilter.empty() || scopeFilter == "layer";

    std::ostringstream oss;

    if (wantStage) {
        UsdStageRefPtr stage = _stageFn ? _stageFn() : UsdStageRefPtr();
        if (stage) {
            auto paths = sel->GetSelectedPaths(stage);
            oss << "Stage selection (" << paths.size() << " prim";
            if (paths.size() != 1) oss << "s";
            oss << "):\n";
            if (paths.empty()) {
                oss << "  (none)\n";
            } else {
                for (const SdfPath& p : paths) {
                    oss << "  " << p.GetString() << "\n";
                }
            }
        } else {
            oss << "Stage selection: <no active stage>\n";
        }
    }

    if (wantLayer) {
        SdfLayerRefPtr layer = _editLayerFn ? _editLayerFn() : SdfLayerRefPtr();
        if (layer) {
            // Selection::GetSelectedPaths is specialized on SdfLayerHandle.
            SdfLayerHandle handle(layer);
            auto paths = sel->GetSelectedPaths(handle);
            oss << "Layer selection (edit layer: " << _LayerName(layer)
                << ", " << paths.size() << " prim";
            if (paths.size() != 1) oss << "s";
            oss << "):\n";
            if (paths.empty()) {
                oss << "  (none)\n";
            } else {
                for (const SdfPath& p : paths) {
                    oss << "  " << p.GetString() << "\n";
                }
            }
        } else {
            oss << "Layer selection: <no current edit layer>\n";
        }
    }

    return oss.str();
}

// --------------------------------------------------------------------------
// 13. select_prims  (queued on UI thread; not undoable)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SelectPrims(const JsObject& args) const {
    if (!_selectionFn) return "[error] selection provider not configured";
    Selection* sel = _selectionFn();
    if (!sel) return "[error] no active selection";

    const std::string scope  = JsGetString(args, "scope", "stage");
    const bool        extend = JsGetBool(args, "extend", false);

    if (scope != "stage" && scope != "layer") {
        return "[error] 'scope' must be \"stage\" or \"layer\"; got \""
               + scope + "\"";
    }

    // Parse the paths array.
    if (!JsHasKey(args, "paths")) {
        return "[error] missing 'paths' argument (array of SdfPath strings; "
               "use [] to clear the selection)";
    }
    JsArray rawPaths = JsGetArray(args, "paths");
    std::vector<SdfPath> paths;
    paths.reserve(rawPaths.size());
    for (const JsValue& v : rawPaths) {
        if (!v.IsString()) {
            return "[error] every entry in 'paths' must be an SdfPath string";
        }
        const std::string s = v.GetString();
        if (!SdfPath::IsValidPathString(s)) {
            return "[error] not a valid SdfPath: '" + s + "'";
        }
        paths.push_back(SdfPath(s));
    }

    std::ostringstream summary;
    summary << "Queued: select scope=" << scope
            << " extend=" << (extend ? "true" : "false")
            << " (" << paths.size() << " path"
            << (paths.size() == 1 ? "" : "s") << ")";
    if (!paths.empty()) {
        summary << ":";
        for (size_t i = 0; i < paths.size() && i < 8; ++i) {
            summary << " " << paths[i].GetString();
        }
        if (paths.size() > 8) summary << " ...";
    }
    summary << ". Re-read with get_selection on next step to confirm.";

    if (scope == "stage") {
        UsdStageRefPtr stage = _stageFn ? _stageFn() : UsdStageRefPtr();
        if (!stage) return "[error] no active stage";
        QueueOnUIThread([sel, stage, paths, extend]() {
            if (!extend) sel->Clear(stage);
            for (const SdfPath& p : paths) {
                sel->AddSelected(stage, p);
            }
        });
    } else {
        SdfLayerRefPtr layer = _editLayerFn ? _editLayerFn() : SdfLayerRefPtr();
        if (!layer) return "[error] no current edit layer";
        QueueOnUIThread([sel, layer, paths, extend]() {
            if (!extend) sel->Clear(layer);
            for (const SdfPath& p : paths) {
                sel->AddSelected(layer, p);
            }
        });
    }

    return summary.str();
}

// --------------------------------------------------------------------------
// 13. create_prim  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::CreatePrim(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!path.IsPrimPath()) {
        return "[error] 'path' must be a prim path (e.g. \"/World/Hero\")";
    }

    // Parse specifier — default "def".
    const std::string specStr = JsGetString(args, "specifier");
    SdfSpecifier specifier = SdfSpecifierDef;
    std::string specifierLabel = "def";
    if (specStr == "over") {
        specifier = SdfSpecifierOver;
        specifierLabel = "over";
    } else if (specStr == "class") {
        specifier = SdfSpecifierClass;
        specifierLabel = "class";
    } else if (!specStr.empty() && specStr != "def") {
        return "[error] 'specifier' must be \"def\", \"over\", or \"class\"; got \""
               + specStr + "\"";
    }

    // Optional type name (e.g. "Xform", "Mesh", "Camera").
    const std::string typeName = JsGetString(args, "type");

    // Resolve target layer — named via layer_id or current edit target.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;

    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = _LayerName(targetLayer);
    } else {
        targetLayer = _FindLayer(stage, layerIdArg);
        if (!targetLayer) {
            return "[error] layer '" + layerIdArg + "' not found in the stage's "
                   "layer stack. Call get_layer_stack to see available layers.";
        }
        if (!targetLayer->PermissionToEdit()) {
            return "[error] layer '" + layerIdArg + "' is read-only";
        }
        targetLayerName = _LayerName(targetLayer);
    }

    if (!targetLayer) return "[error] no edit target layer";

    if (targetLayer->GetPrimAtPath(path)) {
        return "[error] a spec already exists at " + path.GetString()
               + " in " + targetLayerName
               + ". Use set_attribute or set_xform to modify it.";
    }

    const std::string ident = targetLayer->GetIdentifier();

    std::function<void()> fn = [stage, path, specifier, typeName, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;

        // Collect ancestor paths that are missing in this layer (deepest first).
        std::vector<SdfPath> missing;
        SdfPath cur = path.GetParentPath();
        while (!cur.IsAbsoluteRootPath() && !layer->GetPrimAtPath(cur)) {
            missing.push_back(cur);
            cur = cur.GetParentPath();
        }

        // Create ancestors shallow-first as typeless "over" specs.
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            const SdfPath& ancPath = *it;
            if (ancPath.IsRootPrimPath()) {
                SdfPrimSpecHandle spec =
                    SdfPrimSpec::New(layer, ancPath.GetName(), SdfSpecifierOver);
                layer->InsertRootPrim(spec);
            } else {
                SdfPrimSpecHandle parent =
                    layer->GetPrimAtPath(ancPath.GetParentPath());
                if (!parent) return;
                SdfPrimSpec::New(parent, ancPath.GetName(), SdfSpecifierOver);
            }
        }

        // Create the target prim spec.
        SdfPrimSpecHandle newSpec;
        if (path.IsRootPrimPath()) {
            newSpec = SdfPrimSpec::New(layer, path.GetName(), specifier);
            layer->InsertRootPrim(newSpec);
        } else {
            SdfPrimSpecHandle parent = layer->GetPrimAtPath(path.GetParentPath());
            if (!parent) return;
            newSpec = SdfPrimSpec::New(parent, path.GetName(), specifier);
        }

        if (newSpec && !typeName.empty()) {
            newSpec->SetTypeName(typeName);
        }
    };

    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: create " << specifierLabel << " prim at " << path.GetString();
    if (!typeName.empty()) oss << " (type=" << typeName << ")";
    oss << " on layer " << targetLayerName
        << ". Re-read with get_prim_info to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// Composition arc helpers — shared by add_reference, add_payload,
// add_inherit, add_specialize.
// --------------------------------------------------------------------------
namespace {

// Resolve the target layer for a composition arc tool: named via layer_id or
// the current edit target. Returns null and fills errorOut on failure.
SdfLayerHandle _ResolveArcLayer(const UsdStageRefPtr& stage,
                                const JsObject& args,
                                std::string& layerNameOut,
                                std::string& errorOut) {
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle layer;
    if (layerIdArg.empty()) {
        layer = stage->GetEditTarget().GetLayer();
        layerNameOut = _LayerName(layer);
    } else {
        layer = _FindLayer(stage, layerIdArg);
        if (!layer) {
            errorOut = "[error] layer '" + layerIdArg
                     + "' not found. Call get_layer_stack first.";
            return {};
        }
        if (!layer->PermissionToEdit()) {
            errorOut = "[error] layer '" + layerIdArg + "' is read-only";
            return {};
        }
        layerNameOut = _LayerName(layer);
    }
    if (!layer) errorOut = "[error] no edit target layer";
    return layer;
}

} // anonymous namespace

// --------------------------------------------------------------------------
// 14. add_reference  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddReference(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath primPath = _GetPath(args);
    if (primPath.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!primPath.IsPrimPath())
        return "[error] 'path' must be a prim path (e.g. \"/World/Hero\")";

    const std::string assetPath = JsGetString(args, "asset_path");

    SdfPath targetPrimPath;
    const std::string primPathStr = JsGetString(args, "prim_path");
    if (!primPathStr.empty()) {
        if (!SdfPath::IsValidPathString(primPathStr))
            return "[error] 'prim_path' is not a valid SdfPath: " + primPathStr;
        targetPrimPath = SdfPath(primPathStr);
    }

    const double offset = JsGetDouble(args, "layer_offset", 0.0);
    const double scale  = JsGetDouble(args, "layer_scale",  1.0);

    std::string layerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, layerName, err);
    if (!targetLayer) return err;

    const std::string ident = targetLayer->GetIdentifier();
    SdfReference ref(assetPath, targetPrimPath, SdfLayerOffset(offset, scale));

    std::function<void()> fn = [primPath, ref, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, primPath);
        if (spec) spec->GetReferenceList().Prepend(ref);
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add reference on " << primPath.GetString()
        << " -> asset=\"" << assetPath << "\"";
    if (!targetPrimPath.IsEmpty()) oss << " prim=" << targetPrimPath.GetString();
    if (offset != 0.0 || scale != 1.0)
        oss << " offset=" << offset << " scale=" << scale;
    oss << " on layer " << layerName
        << ". Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 15. add_payload  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddPayload(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath primPath = _GetPath(args);
    if (primPath.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!primPath.IsPrimPath())
        return "[error] 'path' must be a prim path (e.g. \"/World/Hero\")";

    const std::string assetPath = JsGetString(args, "asset_path");

    SdfPath targetPrimPath;
    const std::string primPathStr = JsGetString(args, "prim_path");
    if (!primPathStr.empty()) {
        if (!SdfPath::IsValidPathString(primPathStr))
            return "[error] 'prim_path' is not a valid SdfPath: " + primPathStr;
        targetPrimPath = SdfPath(primPathStr);
    }

    const double offset = JsGetDouble(args, "layer_offset", 0.0);
    const double scale  = JsGetDouble(args, "layer_scale",  1.0);

    std::string layerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, layerName, err);
    if (!targetLayer) return err;

    const std::string ident = targetLayer->GetIdentifier();
    SdfPayload payload(assetPath, targetPrimPath, SdfLayerOffset(offset, scale));

    std::function<void()> fn = [primPath, payload, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, primPath);
        if (spec) spec->GetPayloadList().Prepend(payload);
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add payload on " << primPath.GetString()
        << " -> asset=\"" << assetPath << "\"";
    if (!targetPrimPath.IsEmpty()) oss << " prim=" << targetPrimPath.GetString();
    if (offset != 0.0 || scale != 1.0)
        oss << " offset=" << offset << " scale=" << scale;
    oss << " on layer " << layerName
        << ". Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 16. add_inherit  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddInherit(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath primPath = _GetPath(args);
    if (primPath.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!primPath.IsPrimPath())
        return "[error] 'path' must be a prim path";

    const std::string targetStr = JsGetString(args, "target_path");
    if (targetStr.empty()) return "[error] missing 'target_path' argument";
    if (!SdfPath::IsValidPathString(targetStr))
        return "[error] 'target_path' is not a valid SdfPath: " + targetStr;
    const SdfPath targetPath(targetStr);
    if (!targetPath.IsAbsolutePath())
        return "[error] 'target_path' must be an absolute path";

    std::string layerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, layerName, err);
    if (!targetLayer) return err;

    const std::string ident = targetLayer->GetIdentifier();

    std::function<void()> fn = [primPath, targetPath, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, primPath);
        if (spec) spec->GetInheritPathList().Prepend(targetPath);
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add inherit on " << primPath.GetString()
        << " <- " << targetPath.GetString()
        << " on layer " << layerName
        << ". Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 17. add_specialize  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddSpecialize(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath primPath = _GetPath(args);
    if (primPath.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!primPath.IsPrimPath())
        return "[error] 'path' must be a prim path";

    const std::string targetStr = JsGetString(args, "target_path");
    if (targetStr.empty()) return "[error] missing 'target_path' argument";
    if (!SdfPath::IsValidPathString(targetStr))
        return "[error] 'target_path' is not a valid SdfPath: " + targetStr;
    const SdfPath targetPath(targetStr);
    if (!targetPath.IsAbsolutePath())
        return "[error] 'target_path' must be an absolute path";

    std::string layerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, layerName, err);
    if (!targetLayer) return err;

    const std::string ident = targetLayer->GetIdentifier();

    std::function<void()> fn = [primPath, targetPath, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, primPath);
        if (spec) spec->GetSpecializesList().Prepend(targetPath);
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add specialize on " << primPath.GetString()
        << " <- " << targetPath.GetString()
        << " on layer " << layerName
        << ". Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 18. add_sublayer  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddSublayer(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    const std::string sublayerPath = JsGetString(args, "sublayer_path");
    if (sublayerPath.empty()) return "[error] missing 'sublayer_path' argument";

    // Default to root layer; override with layer_id.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;

    if (layerIdArg.empty()) {
        targetLayer = stage->GetRootLayer();
        targetLayerName = _LayerName(targetLayer);
    } else {
        targetLayer = _FindLayer(stage, layerIdArg);
        if (!targetLayer) {
            return "[error] layer '" + layerIdArg
                 + "' not found. Call get_layer_stack first.";
        }
        if (!targetLayer->PermissionToEdit()) {
            return "[error] layer '" + layerIdArg + "' is read-only";
        }
        targetLayerName = _LayerName(targetLayer);
    }
    if (!targetLayer) return "[error] no target layer";

    for (const std::string& p : targetLayer->GetSubLayerPaths()) {
        if (p == sublayerPath) {
            return "[error] \"" + sublayerPath + "\" is already a sublayer of "
                 + targetLayerName;
        }
    }

    const std::string ident = targetLayer->GetIdentifier();

    std::function<void()> fn = [sublayerPath, ident]() {
        SdfLayerRefPtr layer = SdfLayer::Find(ident);
        if (!layer) return;
        layer->InsertSubLayerPath(sublayerPath, 0);
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add sublayer \"" << sublayerPath << "\" to " << targetLayerName
        << ". Re-read with get_layer_stack to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 19. get_relationship_targets  (read-only)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetRelationshipTargets(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";

    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) return "[error] no prim at " + path.GetString();

    const std::string relName = JsGetString(args, "name");
    if (relName.empty()) return "[error] missing 'name' argument";

    UsdRelationship rel = prim.GetRelationship(TfToken(relName));
    if (!rel) {
        return "[error] no relationship '" + relName + "' on " + path.GetString()
             + ". Check get_prim_info for the list of property names.";
    }

    SdfPathVector targets;
    rel.GetTargets(&targets);

    std::ostringstream oss;
    oss << "Relationship: " << path.GetString() << "." << relName << "\n";
    oss << "Targets (" << targets.size() << "):\n";
    if (targets.empty()) {
        oss << "  (none)\n";
    } else {
        for (const SdfPath& t : targets) oss << "  " << t.GetString() << "\n";
    }
    return oss.str();
}

// --------------------------------------------------------------------------
// 20. set_relationship  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetRelationship(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath primPath = _GetPath(args);
    if (primPath.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!primPath.IsPrimPath())
        return "[error] 'path' must be a prim path (e.g. \"/World/Hero\")";

    const std::string relName = JsGetString(args, "name");
    if (relName.empty()) return "[error] missing 'name' argument";

    // Parse targets array.
    JsArray rawTargets = JsGetArray(args, "targets");
    SdfPathVector targets;
    targets.reserve(rawTargets.size());
    for (const JsValue& v : rawTargets) {
        if (!v.IsString())
            return "[error] every entry in 'targets' must be an SdfPath string";
        const std::string s = v.GetString();
        if (!SdfPath::IsValidPathString(s))
            return "[error] not a valid SdfPath: '" + s + "'";
        targets.push_back(SdfPath(s));
    }

    // Parse operation: 0=prepend (default), 1=append, 2=explicit (replace all).
    const std::string opStr = JsGetString(args, "operation");
    int opCode = 0;
    if      (opStr == "append")   opCode = 1;
    else if (opStr == "explicit") opCode = 2;
    else if (!opStr.empty() && opStr != "prepend")
        return "[error] 'operation' must be \"prepend\", \"append\", or \"explicit\"";

    // Resolve target layer.
    std::string layerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, layerName, err);
    if (!targetLayer) return err;

    const std::string ident   = targetLayer->GetIdentifier();
    const std::string opLabel = (opCode==1) ? "append" : (opCode==2) ? "explicit" : "prepend";

    std::function<void()> fn = [primPath, relName, targets, opCode, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;

        SdfPrimSpecHandle prim = _EnsurePrimSpec(layer, primPath);
        if (!prim) return;

        // Get existing relationship spec or create a new one.
        SdfPath relSpecPath = primPath.AppendProperty(TfToken(relName));
        SdfRelationshipSpecHandle rel = layer->GetRelationshipAtPath(relSpecPath);
        if (!rel) {
            rel = SdfRelationshipSpec::New(prim, relName,
                                          /*custom=*/false,
                                          SdfVariabilityUniform);
        }
        if (!rel) return;

        if (opCode == 2) {
            // Explicit: replace all targets.
            rel->GetTargetPathList().ClearEdits();
            for (const SdfPath& t : targets) rel->GetTargetPathList().Append(t);
        } else if (opCode == 0) {
            // Prepend: add in reverse order so first target ends up strongest.
            for (auto it = targets.rbegin(); it != targets.rend(); ++it)
                rel->GetTargetPathList().Prepend(*it);
        } else {
            // Append.
            for (const SdfPath& t : targets) rel->GetTargetPathList().Append(t);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: set relationship " << primPath.GetString() << "." << relName
        << " op=" << opLabel << " targets=[";
    for (size_t i = 0; i < targets.size(); ++i) {
        if (i) oss << ", ";
        oss << targets[i].GetString();
    }
    oss << "] on layer " << layerName
        << ". Re-read with get_relationship_targets to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 21. delete_prim  (queued)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::DeletePrim(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfPath path = _GetPath(args);
    if (path.IsEmpty()) return "[error] missing or invalid 'path' argument";
    if (!path.IsPrimPath())
        return "[error] 'path' must be a prim path (e.g. \"/World/Hero\")";

    // Resolve target layer — named via layer_id or current edit target.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;

    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = _LayerName(targetLayer);
    } else {
        targetLayer = _FindLayer(stage, layerIdArg);
        if (!targetLayer) {
            return "[error] layer '" + layerIdArg
                 + "' not found. Call get_layer_stack first.";
        }
        if (!targetLayer->PermissionToEdit()) {
            return "[error] layer '" + layerIdArg + "' is read-only";
        }
        targetLayerName = _LayerName(targetLayer);
    }
    if (!targetLayer) return "[error] no edit target layer";

    if (!targetLayer->GetPrimAtPath(path)) {
        return "[error] no spec at " + path.GetString()
             + " in " + targetLayerName
             + ". The prim may exist in a different layer — call get_layer_stack "
               "and use layer_id to target the right one.";
    }

    const std::string ident = targetLayer->GetIdentifier();

    std::function<void()> fn = [path, ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
        if (!spec) return;
        if (SdfPrimSpecHandle parent = spec->GetNameParent()) {
            parent->RemoveNameChild(spec);
        } else {
            layer->RemoveRootPrim(spec);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: delete spec at " << path.GetString()
        << " from layer " << targetLayerName
        << ". Note: if other layers hold opinions on this prim it will still "
           "appear on the composed stage. Re-read with get_prim_info to confirm.";
    return oss.str();
}

} // namespace UsdAgent
