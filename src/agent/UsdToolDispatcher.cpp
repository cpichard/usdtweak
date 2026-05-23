#include "UsdToolDispatcher.h"

#include "Commands.h"
#include "JsHelpers.h"
#include "Selection.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/attributeQuery.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/resolveInfo.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/imageable.h>

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
        if      (toolName == "get_prim_info")        result = GetPrimInfo(args);
        else if (toolName == "get_attribute_value")  result = GetAttributeValue(args);
        else if (toolName == "get_value_resolution") result = GetValueResolution(args);
        else if (toolName == "get_composition_arcs") result = GetCompositionArcs(args);
        else if (toolName == "get_layer_stack")      result = GetLayerStack(args);
        else if (toolName == "list_children")        result = ListChildren(args);
        else if (toolName == "find_prims")           result = FindPrims(args);
        else if (toolName == "set_attribute")        result = SetAttribute(args);
        else if (toolName == "set_active")           result = SetActive(args);
        else if (toolName == "set_variant")          result = SetVariant(args);
        else if (toolName == "set_visibility")       result = SetVisibility(args);
        else if (toolName == "get_selection")        result = GetSelection(args);
        else if (toolName == "select_prims")         result = SelectPrims(args);
        else                                         result = "[error] unknown tool: " + toolName;
    } catch (const std::exception& e) {
        result = std::string("[error] ") + e.what();
    } catch (...) {
        result = "[error] unknown exception in tool " + toolName;
    }
    return _CapResult(std::move(result));
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
// Edit-tool helpers
// --------------------------------------------------------------------------
namespace {

// Coerce a string into a VtValue matching the attribute's declared type.
// V1: scalar simple types only. Vector types (vec3, color3f, …), array
// types, and matrix types return an [error] string for now — the agent
// can re-issue with a smaller scalar attribute or wait for v2.
//
// Returns true on success and writes to *out. On failure writes the reason
// into *errOut.
bool _ParseValueForAttribute(const std::string& text,
                             const SdfValueTypeName& type,
                             VtValue* out,
                             std::string* errOut) {
    if (!type) { *errOut = "attribute has no declared type"; return false; }

    const TfToken typeName = type.GetAsToken();
    const std::string t = typeName.GetString();

    auto strToBool = [&](bool& b) {
        if (text == "true" || text == "1" || text == "True")  { b = true;  return true; }
        if (text == "false"|| text == "0" || text == "False") { b = false; return true; }
        return false;
    };

    if (t == "bool") {
        bool b;
        if (!strToBool(b)) {
            *errOut = "expected bool ('true'/'false'), got '" + text + "'";
            return false;
        }
        *out = VtValue(b); return true;
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
    if (t == "string") {
        *out = VtValue(text); return true;
    }
    if (t == "token") {
        *out = VtValue(TfToken(text)); return true;
    }
    if (t == "asset") {
        *out = VtValue(SdfAssetPath(text)); return true;
    }

    *errOut = "value type '" + t + "' is not supported by set_attribute in v1 "
              "(scalar bool/int/float/double/string/token/asset only)";
    return false;
}

} // namespace

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

    ExecuteAfterDraw<AttributeSet>(attr, value, time);

    std::ostringstream oss;
    oss << "Queued: set " << path.GetString() << "." << attrName
        << " (" << attr.GetTypeName().GetAsToken().GetString() << ") = "
        << valueText
        << " @ " << (time.IsDefault() ? "DEFAULT"
                                       : TfStringPrintf("%g", time.GetValue()))
        << " on edit target "
        << (stage->GetEditTarget().GetLayer()
            ? stage->GetEditTarget().GetLayer()->GetIdentifier()
            : std::string("<none>"))
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

} // namespace UsdAgent
