#include "UsdToolDispatcher.h"

#include "Commands.h"
#include "JsHelpers.h"
#include "Selection.h"

// UTQL query engine (compiled into the usdtweak target / the test agent
// sources). We use only the synchronous compile+execute path — Parse, Bind,
// Execute — never UtqlEngine, which is the UI-thread async marshaller.
#include "utql/Binder.h"
#include "utql/Executor.h"
#include "utql/Parser.h"
#include "utql/UtqlTypes.h"

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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

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

// ----- name tokenization (shared by find_prims and get_name_vocabulary) ----

std::string _ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

// Tokenize a prim's LOCAL name into lowercased lexical tokens. Emits the
// camelCase/separator split pieces PLUS the "stripped full name" (all
// alphabetic content concatenated, numbers & separators removed), deduped.
// Single-character and pure-numeric tokens are dropped.
//   "Towel_1"     -> {"towel"}                    (single piece == full name)
//   "Kitchen_001" -> {"kitchen"}
//   "ProxyMesh"   -> {"proxymesh","proxy","mesh"} (full + pieces)
//   "Left_Arm_Rig"-> {"leftarmrig","left","arm","rig"}
//   "Props"       -> {"props"}
//   "TOwell"      -> {"towell"}                    (no lower->upper split)
//
// Splitting: on separators (_ - . : space) and on lower->upper transitions
// only. The lower->upper-only rule keeps acronym-prefixed names like "TOwell"
// whole (avoids emitting a junk "owell") while still splitting genuine
// camelCase ("ProxyMesh" -> proxy, mesh).
std::vector<std::string> _TokenizeName(const std::string& name) {
    auto isSep = [](char c){
        return c == '_' || c == '-' || c == '.' || c == ':' ||
               std::isspace(static_cast<unsigned char>(c));
    };

    // 1. Split into alpha-numeric fragments on separators.
    std::vector<std::string> fragments;
    std::string cur;
    for (char c : name) {
        if (isSep(c)) { if (!cur.empty()) { fragments.push_back(cur); cur.clear(); } }
        else           cur.push_back(c);
    }
    if (!cur.empty()) fragments.push_back(cur);

    // 2. Within each fragment, split on a lower->upper transition; collect the
    //    pieces. Also accumulate the stripped full name (alpha only) across
    //    all fragments.
    std::vector<std::string> pieces;
    std::string fullAlpha;
    for (const std::string& frag : fragments) {
        std::string piece;
        char prev = 0;
        for (char c : frag) {
            const bool lowerToUpper =
                prev != 0 &&
                std::islower(static_cast<unsigned char>(prev)) &&
                std::isupper(static_cast<unsigned char>(c));
            if (lowerToUpper && !piece.empty()) { pieces.push_back(piece); piece.clear(); }
            piece.push_back(c);
            if (std::isalpha(static_cast<unsigned char>(c))) fullAlpha.push_back(c);
            prev = c;
        }
        if (!piece.empty()) pieces.push_back(piece);
    }

    // 3. Lowercase, drop empty / single-char / all-digit tokens, dedupe.
    //    Emit the stripped full name first (when it differs from the pieces).
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& raw){
        if (raw.size() < 2) return;                         // drop single char
        if (std::all_of(raw.begin(), raw.end(),
                        [](unsigned char c){ return std::isdigit(c); })) return;
        const std::string tok = _ToLower(raw);
        if (seen.insert(tok).second) out.push_back(tok);
    };
    add(fullAlpha);
    for (const std::string& p : pieces) add(p);
    return out;
}

// ----- named prim lists: canonical form ------------------------------------

// Reduce a path vector to its canonical set form: sorted (SdfPath::operator<)
// and de-duplicated in place. Every stored list is kept in this form so that
// set semantics are an invariant, read_list pagination is deterministic, and
// combine() is a linear std::set_* merge.
void _Normalize(std::vector<SdfPath>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

} // namespace

UsdToolDispatcher::UsdToolDispatcher(StageProvider     stageFn,
                                     EditLayerProvider editLayerFn,
                                     SelectionProvider selectionFn,
                                     OpenFileProvider  openFileFn)
    : _stageFn(std::move(stageFn))
    , _editLayerFn(std::move(editLayerFn))
    , _selectionFn(std::move(selectionFn))
    , _openFileFn(std::move(openFileFn)) {}

// ----- named prim list store -----------------------------------------------

void UsdToolDispatcher::_StoreList(const std::string& name,
                                   std::vector<SdfPath> paths) const {
    _Normalize(paths);
    std::lock_guard<std::mutex> lock(_listsMutex);
    _lists[name] = std::move(paths);
}

bool UsdToolDispatcher::_GetListCopy(const std::string& name,
                                     std::vector<SdfPath>& out) const {
    std::lock_guard<std::mutex> lock(_listsMutex);
    auto it = _lists.find(name);
    if (it == _lists.end()) return false;
    out = it->second;   // copy out so callers never touch the store unlocked
    return true;
}

// ----- public read accessors (UI thread / Lists panel) ---------------------

std::vector<std::string> UsdToolDispatcher::GetListNames() const {
    std::lock_guard<std::mutex> lock(_listsMutex);
    std::vector<std::string> names;
    names.reserve(_lists.size());
    for (const auto& kv : _lists)   // std::map keeps keys sorted
        names.push_back(kv.first);
    return names;
}

bool UsdToolDispatcher::GetList(const std::string& name,
                                std::vector<SdfPath>& out) const {
    return _GetListCopy(name, out);
}

size_t UsdToolDispatcher::GetListSize(const std::string& name) const {
    std::lock_guard<std::mutex> lock(_listsMutex);
    auto it = _lists.find(name);
    return it == _lists.end() ? size_t(0) : it->second.size();
}

bool UsdToolDispatcher::_ResolveBatchItems(const JsObject& args, JsArray& out,
                                           std::string& errOut) const {
    const bool hasList  = JsHasKey(args, "list_id");
    const bool hasItems = JsHasKey(args, "items");
    if (hasList && hasItems) {
        errOut = "[error] use either 'list_id' or 'items', not both";
        return false;
    }
    if (!hasList) {
        if (!hasItems) {
            errOut = "[error] missing 'items' (or 'list_id') argument";
            return false;
        }
        out = JsGetArray(args, "items");
        if (out.empty()) {
            errOut = "[error] 'items' is empty — nothing to do";
            return false;
        }
        return true;
    }

    const std::string name = JsGetString(args, "list_id");
    std::vector<SdfPath> paths;
    if (!_GetListCopy(name, paths)) {
        errOut = "[error] no list \"" + name + "\"; call manage_lists "
                 "operation=list to see stored lists, or find_prims with "
                 "store_as to create one";
        return false;
    }
    if (paths.empty()) {
        errOut = "[error] list \"" + name + "\" is empty — nothing to do";
        return false;
    }
    out = JsArray();
    out.reserve(paths.size());
    for (const SdfPath& p : paths) {
        JsObject o;
        o["path"] = JsValue(p.GetString());
        out.push_back(JsValue(o));
    }
    return true;
}

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
        else if (toolName == "run_query")            result = RunQuery(args);
        else if (toolName == "read_list")            result = ReadList(args);
        else if (toolName == "manage_lists")         result = ManageLists(args);
        else if (toolName == "get_name_vocabulary")  result = GetNameVocabulary(args);
        else if (toolName == "find_usd_files")       result = FindUsdFiles(args);
        else if (toolName == "set_xforms")           result = SetXforms(args);
        else if (toolName == "set_attributes")       result = SetAttributes(args);
        else if (toolName == "set_actives")          result = SetActives(args);
        else if (toolName == "set_variant")          result = SetVariant(args);
        else if (toolName == "set_visibilities")     result = SetVisibilities(args);
        else if (toolName == "get_selection")        result = GetSelection(args);
        else if (toolName == "select_prims")         result = SelectPrims(args);
        else if (toolName == "create_prims")         result = CreatePrims(args);
        else if (toolName == "add_references")       result = AddReferences(args);
        else if (toolName == "add_payloads")         result = AddPayloads(args);
        else if (toolName == "add_inherits")         result = AddInherits(args);
        else if (toolName == "add_specializes")      result = AddSpecializes(args);
        else if (toolName == "add_sublayer")         result = AddSublayer(args);
        else if (toolName == "delete_prims")         result = DeletePrims(args);
        else if (toolName == "get_relationship_targets") result = GetRelationshipTargets(args);
        else if (toolName == "set_relationship")     result = SetRelationship(args);
        else if (toolName == "open_file")            result = OpenFile(args);
        else if (toolName == "create_layer_file")    result = CreateLayerFile(args);
        else if (toolName == "get_edit_target")      result = GetEditTarget(args);
        else if (toolName == "set_edit_target")      result = SetEditTarget(args);
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

    auto nameAndType = [](const UsdPrim& p) -> std::string {
        std::string s = p.GetName().GetString();
        s += p.GetTypeName().IsEmpty() ? " (untyped)"
                                       : " (" + p.GetTypeName().GetString() + ")";
        return s;
    };

    if (!recursive) {
        int n = 0;
        for (const UsdPrim& c : prim.GetChildren()) {
            oss << "  " << nameAndType(c) << "\n";
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
            << nameAndType(p) << "\n";
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
    const std::string namePattern   = JsGetString(args, "name_pattern");
    // When set, the FULL (uncapped) match set is retained client-side under
    // this handle for later reuse by the edit tools (list_id) and read_list.
    const std::string storeAs       = JsGetString(args, "store_as");

    // Case-insensitive, OR-matched lexical tokens (see get_name_vocabulary).
    std::vector<std::string> nameTokens;
    for (const JsValue& v : JsGetArray(args, "name_tokens"))
        if (v.IsString()) nameTokens.push_back(_ToLower(v.GetString()));

    // Optional subtree scoping: restrict the search to the given prims and
    // their descendants. Accepts an array of prim paths (or a single path
    // string). Overlapping/nested roots are pruned to disjoint subtrees so a
    // prim under two listed roots is still counted exactly once.
    const bool underSpecified = JsHasKey(args, "under");
    std::vector<SdfPath>     roots;
    std::vector<std::string> badRoots;
    {
        auto addRoot = [&](const std::string& s) {
            if (s.empty()) return;
            if (!SdfPath::IsValidPathString(s) || !SdfPath(s).IsAbsolutePath()
                || !SdfPath(s).IsPrimPath()) { badRoots.push_back(s); return; }
            roots.push_back(SdfPath(s));
        };
        if (underSpecified) {
            const JsValue& u = args.at("under");
            if (u.IsString())     addRoot(u.GetString());
            else if (u.IsArray()) for (const JsValue& v : u.GetJsArray())
                                      if (v.IsString()) addRoot(v.GetString());
        }
        // Keep only the shallowest of any nested set so subtrees are disjoint.
        std::sort(roots.begin(), roots.end());
        std::vector<SdfPath> pruned;
        for (const SdfPath& r : roots) {
            bool covered = false;
            for (const SdfPath& k : pruned)
                if (r.HasPrefix(k)) { covered = true; break; }
            if (!covered) pruned.push_back(r);
        }
        roots.swap(pruned);
    }

    std::ostringstream oss;
    oss << "find_prims: ";
    if (!typeFilter.empty())    oss << "type=" << typeFilter << " ";
    if (!kindFilter.empty())    oss << "kind=" << kindFilter << " ";
    if (!purposeFilter.empty()) oss << "purpose=" << purposeFilter << " ";
    if (hasActive)              oss << "active=" << (activeFilter ? "true" : "false") << " ";
    if (!namePattern.empty())   oss << "name_pattern=" << namePattern << " ";
    if (!nameTokens.empty()) {
        oss << "name_tokens=[";
        for (size_t i = 0; i < nameTokens.size(); ++i)
            oss << (i ? "," : "") << nameTokens[i];
        oss << "] ";
    }
    if (!roots.empty()) {
        oss << "under=[";
        for (size_t i = 0; i < roots.size(); ++i)
            oss << (i ? "," : "") << roots[i].GetString();
        oss << "] ";
    }
    if (typeFilter.empty() && kindFilter.empty() && purposeFilter.empty()
        && !hasActive && namePattern.empty() && nameTokens.empty()
        && !underSpecified) {
        oss << "(no filters — listing all prims)";
    }
    oss << "\n";

    // Collect matches (up to kFindPrimsLimit) for display, but count every
    // match in matchedTotal so the footer never understates the real scale
    // (the LLM must know it is seeing a fraction, not the whole set).
    std::vector<UsdPrim> results;
    results.reserve(kFindPrimsLimit);
    // Full match set, only populated when storing — this is what bypasses the
    // display cap so the edit tools can act on all matches, not just the 50.
    std::vector<SdfPath> full;
    size_t scanned      = 0;
    size_t matchedTotal = 0;
    bool   truncated    = false;

    // Per-prim filter + collect. Returns early (not continue) on a miss so it
    // can be driven over either the whole stage or a set of subtrees.
    auto consider = [&](const UsdPrim& prim) {
        ++scanned;
        // name_tokens: cheap and usually the most discriminating — check first.
        if (!nameTokens.empty()) {
            const std::vector<std::string> primToks =
                _TokenizeName(prim.GetName().GetString());
            bool any = false;
            for (const std::string& q : nameTokens)
                if (std::find(primToks.begin(), primToks.end(), q) != primToks.end()) {
                    any = true; break;
                }
            if (!any) return;
        }
        if (!namePattern.empty() &&
            prim.GetName().GetString().find(namePattern) == std::string::npos) return;
        if (!typeFilter.empty() &&
            prim.GetTypeName().GetString() != typeFilter) return;
        if (!kindFilter.empty()) {
            TfToken k;
            if (!UsdModelAPI(prim).GetKind(&k) || k.GetString() != kindFilter)
                return;
        }
        if (!purposeFilter.empty()) {
            UsdGeomImageable img(prim);
            TfToken p;
            if (!img || !img.GetPurposeAttr().Get(&p) ||
                p.GetString() != purposeFilter) return;
        }
        if (hasActive && prim.IsActive() != activeFilter) return;

        ++matchedTotal;
        if (!storeAs.empty()) full.push_back(prim.GetPath());   // every match
        if (results.size() >= kFindPrimsLimit) { truncated = true; return; }
        results.push_back(prim);
    };

    if (underSpecified && roots.empty()) {
        // 'under' was given but every root was invalid/unresolved. Do NOT fall
        // back to a full-stage scan — that would be a surprising blast radius.
    } else if (roots.empty()) {
        for (const UsdPrim& prim : stage->Traverse()) consider(prim);
    } else {
        // Walk each disjoint subtree (the prim and its descendants). This is
        // also cheaper than a whole-stage scan when scoped to a small subtree.
        for (const SdfPath& r : roots) {
            UsdPrim rp = stage->GetPrimAtPath(r);
            if (!rp) { badRoots.push_back(r.GetString()); continue; }
            for (const UsdPrim& prim : UsdPrimRange(rp)) consider(prim);
        }
    }

    // With 2+ results, compute the common ancestor. If it has depth >= 1 (i.e.
    // not the pseudo-root) print it as a header and strip it from each entry,
    // saving repeated prefix tokens. Single-result queries keep their full path.
    SdfPath commonPrefix;
    if (results.size() >= 2) {
        commonPrefix = results[0].GetPath();
        for (size_t i = 1; i < results.size(); ++i)
            commonPrefix = commonPrefix.GetCommonPrefix(results[i].GetPath());
    }
    const bool usePrefix = commonPrefix.GetPathElementCount() >= 1;
    if (usePrefix)
        oss << "base: " << commonPrefix.GetString() << "\n";

    for (const UsdPrim& prim : results) {
        std::string display;
        if (usePrefix) {
            const std::string full   = prim.GetPath().GetString();
            const std::string& pfx   = commonPrefix.GetString();
            display = (full == pfx) ? std::string(".") : full.substr(pfx.size() + 1);
        } else {
            display = prim.GetPath().GetString();
        }
        const std::string typeName = prim.GetTypeName().GetString();
        display += typeName.empty() ? " (untyped)" : " (" + typeName + ")";
        oss << "  - " << display << "\n";
    }

    if (truncated)
        oss << "matched " << matchedTotal << " prims, showing first "
            << kFindPrimsLimit << " (" << scanned << " scanned)\n";
    else
        oss << "matched " << matchedTotal << " of " << scanned << " prims\n";

    if (!badRoots.empty()) {
        oss << "note: " << badRoots.size()
            << " 'under' path(s) invalid or not found: ";
        for (size_t i = 0; i < badRoots.size(); ++i)
            oss << (i ? ", " : "") << badRoots[i];
        oss << "\n";
    }

    if (!storeAs.empty()) {
        const size_t n = full.size();   // == matchedTotal, before normalize
        _StoreList(storeAs, std::move(full));
        oss << "stored " << n << " path" << (n == 1 ? "" : "s")
            << " as \"" << storeAs << "\" (use list_id=\"" << storeAs
            << "\" on edit tools, or read_list to page through)\n";
    }
    return oss.str();
}

// --------------------------------------------------------------------------
// 7b. run_query — compile + execute a UTQL query against the active stage.
//
// This is the expressive complement to find_prims: it reaches features find_prims
// structurally cannot (VALUE.SCALAR, CONNECTED TO, ISINSTANCE, RELATIONSHIPS,
// HAS_TIMESAMPLES, asset-missing, …). It runs the SYNCHRONOUS utql path on the
// dispatcher's own worker thread — Parse → Bind → Execute — never UtqlEngine
// (which exists only to marshal async results back to the ImGui frame loop).
//
// Scope: composed Stage entities (USDPRIM/USDATTRIBUTE/USDRELATIONSHIP) plus
// FIND LAYER, which scans the active stage's used-layer set (sublayers +
// referenced/payload layers). The authored per-spec SDF* entities and
// CONTRIBUTING TO are still rejected — they need per-layer authoring context this
// single-stage tool doesn't expose. A CompileError is recoverable — the model
// reads the message, fixes the query, and retries.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::RunQuery(const JsObject& args) const {
    const std::string query   = JsGetString(args, "query");
    if (query.empty()) return "[error] missing 'query' argument";
    // When set, the FULL result path set is retained client-side under this
    // handle for reuse by the edit tools (list_id) and read_list — exactly like
    // find_prims store_as, so a query result composes with the batched edits.
    const std::string storeAs = JsGetString(args, "store_as");

    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    // Compile. Parse failures carry a column; bind failures a plain message.
    utql::Query ast;
    std::string err;
    size_t      errPos = 0;
    if (!utql::Parse(query, ast, err, errPos))
        return "[error] compile: " + err + " (at column "
               + std::to_string(errPos + 1) + ") — fix the query and retry";
    utql::BoundQuery bound;
    if (!utql::Bind(std::move(ast), bound, err))
        return "[error] compile: " + err + " — fix the query and retry";

    // Scope guard: composed Stage entities plus FIND LAYER. The authored
    // per-spec SDF* entities (SDFPRIM/SDFATTRIBUTE/SDFRELATIONSHIP) and
    // CONTRIBUTING TO are still out of scope — they need per-layer authoring
    // context this tool doesn't expose. LAYER is Layer-world but reads only layer
    // metadata, so it runs against the stage's used-layer set below.
    if (bound.world == utql::UtqlWorld::Layer &&
        bound.entity != utql::UtqlEntity::Layer)
        return "[error] this tool runs Stage-world queries (FIND USDPRIM / "
               "USDATTRIBUTE / USDRELATIONSHIP) plus FIND LAYER. The authored "
               "per-spec entities (SDFPRIM, SDFATTRIBUTE, SDFRELATIONSHIP) and "
               "CONTRIBUTING TO are not supported here — use find_prims / the "
               "get_* tools instead.";

    // Stage context: the active stage, plus its used-layer set so FIND LAYER has
    // layers to scan (sublayers + referenced/payload layers). The named-results
    // cache is shared across run_query calls so `AS "x"` / `IN RESULTSET "x"`
    // compose; current time = the context default. Execute only reads USD.
    utql::UtqlContext ctx;
    ctx.currentStage = stage;
    ctx.allStages    = {stage};
    for (const SdfLayerHandle& h : stage->GetUsedLayers(/*includeClipLayers*/ true))
        if (h)
            ctx.allLayers.push_back(SdfLayerRefPtr(h));
    ctx.named        = &_namedResults;

    std::atomic<bool> cancel{false};
    const utql::UtqlResult res = utql::Execute(bound, ctx, cancel);

    // Cache under the AS name so a later query can reference this set. Mirror
    // UtqlEngine::Update: cache only fully-completed runs (Ok / OkEmpty), never
    // a degraded/partial one — chaining off a truncated set would mislead.
    if (!bound.asName.empty() &&
        (res.status == utql::UtqlStatus::Ok ||
         res.status == utql::UtqlStatus::OkEmpty)) {
        _namedResults[bound.asName] = res;
    }

    // Status → behaviour the model should follow (mirrors the UtqlStatus design).
    if (res.status == utql::UtqlStatus::CompileError)
        return "[error] compile: " + res.message + " — fix the query and retry";

    std::ostringstream oss;
    oss << "run_query: " << res.matched << " matched, " << res.scanned
        << " scanned";
    if (res.status == utql::UtqlStatus::OkDegraded)
        oss << " (degraded — the scan was cut short; treat as partial)";
    oss << "\n";

    // If the query named itself with AS, it is now cached for later reference.
    auto appendCacheNote = [&]() {
        if (!bound.asName.empty() &&
            (res.status == utql::UtqlStatus::Ok ||
             res.status == utql::UtqlStatus::OkEmpty))
            oss << "cached as RESULTSET \"" << bound.asName << "\" — a later "
                   "run_query can reference it with IN RESULTSET \"" << bound.asName
                << "\" or PATH UNDER RESULTSET \"" << bound.asName << "\".\n";
    };

    if (res.status == utql::UtqlStatus::OkEmpty || res.rows.empty()) {
        oss << "no rows matched — report 'none found'; do NOT retry the same "
               "query.\n";
        appendCacheNote();
        return oss.str();
    }

    // Column header: the path is always present; RETURN fields follow.
    oss << "columns: path";
    for (const std::string& c : res.columnNames) oss << " | " << c;
    oss << "\n";

    // Rows, capped for display like find_prims. The full set still feeds store_as.
    const size_t shown = std::min<size_t>(res.rows.size(), kFindPrimsLimit);
    for (size_t i = 0; i < shown; ++i) {
        const utql::UtqlRow& row = res.rows[i];
        oss << "  " << row.path.GetString();
        for (const utql::UtqlValue& v : row.columns) oss << " | " << v.ToDisplay();
        oss << "\n";
    }
    if (res.rows.size() > shown)
        oss << "... showing first " << shown << " of " << res.rows.size()
            << " rows\n";

    if (!storeAs.empty()) {
        // store_as is cleanest for prim queries — the named-list store feeds the
        // prim edit tools (list_id). For attribute/relationship entities the
        // stored paths are property paths; flag that so the model doesn't pass
        // them to a prim edit tool by mistake.
        std::vector<SdfPath> paths;
        paths.reserve(res.rows.size());
        for (const utql::UtqlRow& row : res.rows) paths.push_back(row.path);
        const size_t n = paths.size();
        _StoreList(storeAs, std::move(paths));
        oss << "stored " << n << " path" << (n == 1 ? "" : "s") << " as \""
            << storeAs << "\"";
        if (bound.entity != utql::UtqlEntity::UsdPrim)
            oss << " (note: these are " << (bound.entity == utql::UtqlEntity::UsdAttribute
                    ? "attribute" : "property") << " paths, not prim paths — not "
                   "directly usable by the prim edit tools)";
        else
            oss << " (use list_id=\"" << storeAs << "\" on edit tools, or "
                   "read_list to page through)";
        oss << "\n";
    }
    appendCacheNote();
    return oss.str();
}

// --------------------------------------------------------------------------
// 7c. get_name_vocabulary
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetNameVocabulary(const JsObject& args) const {
    (void)args;   // no parameters
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    // token -> number of prims contributing it. Built fresh per call: the
    // stage is mutable, so a cached vocabulary would go stale.
    std::map<std::string, int> counts;
    size_t primCount = 0;
    for (const UsdPrim& prim : stage->Traverse()) {
        ++primCount;
        // _TokenizeName already dedupes within a single name, so each prim
        // contributes at most 1 to a given token's count.
        for (const std::string& tok : _TokenizeName(prim.GetName().GetString()))
            ++counts[tok];
    }

    // Sort by count desc, then alpha, for a stable, scan-friendly list.
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });

    std::ostringstream oss;
    oss << "name_vocabulary: " << sorted.size() << " distinct tokens from "
        << primCount << " prims\n";
    for (const auto& entry : sorted)
        oss << "  " << entry.first << " (" << entry.second << ")\n";
    return oss.str();   // global 8 KB cap (_CapResult) is the backstop
}

// --------------------------------------------------------------------------
// read_list — page through a stored list (closes the tool→model read cap)
//
// Prints paths[offset, offset+limit) plus the total and a next_offset hint.
// Reuses find_prims' common-prefix compression to keep the page cheap. This
// is the ONE tool that deliberately moves stored paths to the model, and only
// ≤ kFindPrimsLimit per call.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::ReadList(const JsObject& args) const {
    const std::string name = JsGetString(args, "list_id");
    if (name.empty()) return "[error] missing 'list_id' argument";

    std::vector<SdfPath> paths;
    if (!_GetListCopy(name, paths))
        return "[error] no list \"" + name + "\"; call manage_lists "
               "operation=list to see stored lists";

    const size_t total = paths.size();
    long offsetArg = static_cast<long>(JsGetDouble(args, "offset", 0.0));
    if (offsetArg < 0) offsetArg = 0;
    const size_t offset = static_cast<size_t>(offsetArg);

    size_t limit = kFindPrimsLimit;
    if (JsHasKey(args, "limit")) {
        long l = static_cast<long>(JsGetDouble(args, "limit", 0.0));
        if (l > 0 && static_cast<size_t>(l) < limit) limit = static_cast<size_t>(l);
    }

    std::ostringstream oss;
    oss << "list \"" << name << "\": " << total << " path"
        << (total == 1 ? "" : "s") << "\n";
    if (offset >= total) {
        oss << "(offset " << offset << " is past the end)\n";
        return oss.str();
    }
    const size_t end = std::min(offset + limit, total);

    // Common-prefix compression over the slice (same idea as find_prims).
    SdfPath commonPrefix;
    if (end - offset >= 2) {
        commonPrefix = paths[offset];
        for (size_t i = offset + 1; i < end; ++i)
            commonPrefix = commonPrefix.GetCommonPrefix(paths[i]);
    }
    const bool usePrefix = commonPrefix.GetPathElementCount() >= 1;
    if (usePrefix) oss << "base: " << commonPrefix.GetString() << "\n";

    oss << "showing [" << offset << ", " << end << "):\n";
    for (size_t i = offset; i < end; ++i) {
        const std::string fullp = paths[i].GetString();
        if (usePrefix) {
            const std::string& pfx = commonPrefix.GetString();
            oss << "  - " << (fullp == pfx ? std::string(".")
                                           : fullp.substr(pfx.size() + 1)) << "\n";
        } else {
            oss << "  - " << fullp << "\n";
        }
    }
    if (end < total)
        oss << "next_offset: " << end << " (" << (total - end) << " remaining)\n";
    return oss.str();
}

// --------------------------------------------------------------------------
// manage_lists — the cold/derive verbs for named lists, multiplexed on an
// `operation` enum (create | combine | delete | list). Op-specific params are
// validated here because JSON Schema can't conditionally require them. None of
// these touch the USD stage (no undo entry) — they only mutate the client-side
// scratchpad.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::ManageLists(const JsObject& args) const {
    const std::string op = JsGetString(args, "operation");
    if (op.empty())
        return "[error] missing 'operation' (create | combine | delete | list)";

    // ---- list : inventory --------------------------------------------------
    if (op == "list") {
        std::lock_guard<std::mutex> lock(_listsMutex);
        if (_lists.empty()) return "no stored lists";
        std::ostringstream oss;
        oss << _lists.size() << " stored list"
            << (_lists.size() == 1 ? "" : "s") << ":\n";
        for (const auto& kv : _lists)
            oss << "  - \"" << kv.first << "\" (" << kv.second.size()
                << " path" << (kv.second.size() == 1 ? "" : "s") << ")\n";
        return oss.str();
    }

    // ---- delete ------------------------------------------------------------
    if (op == "delete") {
        const std::string name = JsGetString(args, "list_id");
        if (name.empty()) return "[error] delete requires 'list_id'";
        std::lock_guard<std::mutex> lock(_listsMutex);
        if (_lists.erase(name) == 0)
            return "[error] no list \"" + name + "\"";
        return "deleted \"" + name + "\"";
    }

    // ---- create : from explicit paths (the only model→client path input) ---
    if (op == "create") {
        const std::string name = JsGetString(args, "store_as");
        if (name.empty()) return "[error] create requires 'store_as'";
        if (!JsHasKey(args, "paths"))
            return "[error] create requires 'paths' (array of SdfPath strings)";
        std::vector<SdfPath> paths;
        for (const JsValue& v : JsGetArray(args, "paths")) {
            if (!v.IsString())
                return "[error] every entry in 'paths' must be an SdfPath string";
            const std::string s = v.GetString();
            if (!SdfPath::IsValidPathString(s))
                return "[error] not a valid SdfPath: \"" + s + "\"";
            paths.push_back(SdfPath(s));
        }
        const size_t raw = paths.size();
        _StoreList(name, std::move(paths));   // normalizes (sort + unique)
        std::vector<SdfPath> stored;
        _GetListCopy(name, stored);
        std::ostringstream oss;
        oss << "created \"" << name << "\" with " << stored.size() << " path"
            << (stored.size() == 1 ? "" : "s");
        if (stored.size() != raw)
            oss << " (" << (raw - stored.size()) << " duplicate"
                << (raw - stored.size() == 1 ? "" : "s") << " dropped)";
        return oss.str();
    }

    // ---- combine : set algebra over stored lists ---------------------------
    if (op == "combine") {
        const std::string subOp   = JsGetString(args, "op");
        const std::string storeAs = JsGetString(args, "store_as");
        if (storeAs.empty()) return "[error] combine requires 'store_as'";
        if (subOp != "union" && subOp != "intersect" && subOp != "difference")
            return "[error] combine 'op' must be \"union\", \"intersect\", or "
                   "\"difference\"; got \"" + subOp + "\"";
        if (!JsHasKey(args, "inputs"))
            return "[error] combine requires 'inputs' (array of list names)";

        std::vector<std::string> inputNames;
        for (const JsValue& v : JsGetArray(args, "inputs"))
            if (v.IsString()) inputNames.push_back(v.GetString());
        if (inputNames.size() < 2)
            return "[error] combine needs at least 2 lists in 'inputs'";

        // Resolve all inputs to sorted-unique vectors up front.
        std::vector<std::vector<SdfPath>> sets;
        sets.reserve(inputNames.size());
        for (const std::string& n : inputNames) {
            std::vector<SdfPath> s;
            if (!_GetListCopy(n, s))
                return "[error] no list \"" + n + "\" (an input to combine)";
            sets.push_back(std::move(s));   // already normalized in the store
        }

        // Fold the chosen set operation left-to-right.
        std::vector<SdfPath> acc = sets[0];
        for (size_t i = 1; i < sets.size(); ++i) {
            std::vector<SdfPath> out;
            if (subOp == "union") {
                std::set_union(acc.begin(), acc.end(),
                               sets[i].begin(), sets[i].end(),
                               std::back_inserter(out));
            } else if (subOp == "intersect") {
                std::set_intersection(acc.begin(), acc.end(),
                                      sets[i].begin(), sets[i].end(),
                                      std::back_inserter(out));
            } else {  // difference: inputs[0] minus all the rest
                std::set_difference(acc.begin(), acc.end(),
                                    sets[i].begin(), sets[i].end(),
                                    std::back_inserter(out));
            }
            acc = std::move(out);
        }

        const size_t n = acc.size();
        _StoreList(storeAs, std::move(acc));
        std::ostringstream oss;
        oss << "combine " << subOp << " -> \"" << storeAs << "\" = " << n
            << " path" << (n == 1 ? "" : "s");
        return oss.str();
    }

    return "[error] unknown operation \"" + op
         + "\" (create | combine | delete | list)";
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
// set_xforms  (queued, batched, via UsdGeomXformCommonAPI)
//
// Top-level optional defaults — `operation`, `value`, `time` — can be set
// once for the whole batch; each item may override them. `path` is always
// per-item. `layer_id` is top-level only so the batch lands in a single
// undoable command.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetXforms(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    // Effective items come from either the explicit `items` array or, when
    // `list_id` is given, the named client-side list (one {path} per entry;
    // non-path fields come from the top-level shared defaults below).
    JsArray itemsArr;
    std::string itemsErr;
    if (!_ResolveBatchItems(args, itemsArr, itemsErr)) return itemsErr;

    auto validOp = [](const std::string& s) {
        return s == "translate" || s == "rotate" || s == "scale";
    };

    // Shared defaults.
    const bool        topHasOp   = JsHasKey(args, "operation");
    const std::string topOp      = JsGetString(args, "operation");
    if (topHasOp && !topOp.empty() && !validOp(topOp)) {
        return "[error] top-level 'operation' must be \"translate\", "
               "\"rotate\", or \"scale\"; got \"" + topOp + "\"";
    }
    const bool   topHasValue = JsHasKey(args, "value");
    JsArray      topValueArr;
    double       topX = 0, topY = 0, topZ = 0;
    if (topHasValue) {
        topValueArr = JsGetArray(args, "value");
        std::string perr;
        if (!_ParseVec3(topValueArr, topX, topY, topZ, perr)) {
            return "[error] top-level 'value': " + perr;
        }
    }
    const bool   topHasTime = JsHasKey(args, "time");
    const double topTime    = JsGetDouble(args, "time", 0.0);

    // Resolve target layer once.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;
    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = targetLayer ? _LayerName(targetLayer) : "<none>";
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

    struct Item {
        SdfPath     path;
        std::string op;
        GfVec3d     value;
        UsdTimeCode time;
    };
    std::vector<Item>        toApply;
    std::vector<std::string> errors;
    toApply.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }
        UsdPrim prim = stage->GetPrimAtPath(primPath);
        if (!prim) { bad(pathStr + ": no prim at this path"); continue; }
        if (!UsdGeomXformable(prim)) {
            bad(pathStr + ": not UsdGeomXformable"); continue;
        }

        // Resolve effective operation.
        const bool        itemHasOp = JsHasKey(item, "operation");
        const std::string op        = itemHasOp ? JsGetString(item, "operation")
                                                : topOp;
        if (!(itemHasOp || topHasOp) || op.empty()) {
            bad(pathStr + ": missing 'operation' (set per-item or top-level)");
            continue;
        }
        if (!validOp(op)) {
            bad(pathStr + ": 'operation' must be \"translate\", \"rotate\", or "
                "\"scale\"; got \"" + op + "\""); continue;
        }

        // Resolve effective value.
        double x = topX, y = topY, z = topZ;
        const bool itemHasValue = JsHasKey(item, "value");
        if (itemHasValue) {
            JsArray arr = JsGetArray(item, "value");
            std::string perr;
            if (!_ParseVec3(arr, x, y, z, perr)) {
                bad(pathStr + ": 'value': " + perr); continue;
            }
        } else if (!topHasValue) {
            bad(pathStr + ": missing 'value' (set per-item or top-level)");
            continue;
        }

        // Resolve effective time.
        UsdTimeCode time = UsdTimeCode::Default();
        if (JsHasKey(item, "time"))      time = UsdTimeCode(JsGetDouble(item, "time", 0.0));
        else if (topHasTime)             time = UsdTimeCode(topTime);

        toApply.push_back({primPath, op, GfVec3d(x, y, z), time});
    }

    if (toApply.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toApply.size();
    UsdStageRefPtr stageCopy = stage;

    std::function<void()> fn = [stageCopy, items = std::move(toApply), ident]() {
        SdfLayerHandle l = SdfLayer::Find(ident);
        if (!l) return;
        UsdEditContext ctx(stageCopy, UsdEditTarget(l));
        for (const auto& it : items) {
            UsdGeomXformCommonAPI api(stageCopy->GetPrimAtPath(it.path));
            if (it.op == "translate") {
                api.SetTranslate(it.value, it.time);
            } else if (it.op == "rotate") {
                api.SetRotate(GfVec3f(static_cast<float>(it.value[0]),
                                      static_cast<float>(it.value[1]),
                                      static_cast<float>(it.value[2])),
                              UsdGeomXformCommonAPI::RotationOrderXYZ,
                              it.time);
            } else {  // scale
                api.SetScale(GfVec3f(static_cast<float>(it.value[0]),
                                     static_cast<float>(it.value[1]),
                                     static_cast<float>(it.value[2])),
                             it.time);
            }
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: set " << okCount << " xform op"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_attribute_value on xformOp:translate / "
           "xformOp:rotateXYZ / xformOp:scale to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 8. set_attributes  (queued, batched)
//
// Top-level optional defaults — `attribute`, `value`, `time` — can be set
// once for the whole batch; each item may override them. `path` is always
// per-item. `layer_id` is top-level only so the batch lands in a single
// undoable command.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetAttributes(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    // Effective items come from either the explicit `items` array or, when
    // `list_id` is given, the named client-side list (one {path} per entry;
    // non-path fields come from the top-level shared defaults below).
    JsArray itemsArr;
    std::string itemsErr;
    if (!_ResolveBatchItems(args, itemsArr, itemsErr)) return itemsErr;

    // Shared defaults at top level.
    const bool        topHasAttr  = JsHasKey(args, "attribute");
    const std::string topAttrName = JsGetString(args, "attribute");
    const bool        topHasValue = JsHasKey(args, "value");
    const std::string topValueStr = JsGetString(args, "value");
    const bool        topHasTime  = JsHasKey(args, "time");
    const double      topTime     = JsGetDouble(args, "time", 0.0);

    // Resolve target layer once.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;
    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = targetLayer ? _LayerName(targetLayer) : "<none>";
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

    struct Item {
        SdfPath     attrPath;
        VtValue     value;
        UsdTimeCode time;
    };
    std::vector<Item>        toApply;
    std::vector<std::string> errors;
    toApply.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }
        UsdPrim prim = stage->GetPrimAtPath(primPath);
        if (!prim) { bad(pathStr + ": no prim at this path"); continue; }

        // Resolve effective attribute name (item overrides top-level).
        const bool        itemHasAttr = JsHasKey(item, "attribute");
        const std::string attrName    = itemHasAttr
            ? JsGetString(item, "attribute")
            : topAttrName;
        if (!(itemHasAttr || topHasAttr) || attrName.empty()) {
            bad(pathStr + ": missing 'attribute' (set per-item or top-level)");
            continue;
        }

        UsdAttribute attr = prim.GetAttribute(TfToken(attrName));
        if (!attr) {
            bad(pathStr + ": no attribute '" + attrName + "'"); continue;
        }

        // Resolve effective value (JsHasKey so empty strings are honored).
        const bool        itemHasValue = JsHasKey(item, "value");
        const std::string valueText    = itemHasValue
            ? JsGetString(item, "value")
            : topValueStr;
        if (!(itemHasValue || topHasValue)) {
            bad(pathStr + "." + attrName
                + ": missing 'value' (set per-item or top-level)");
            continue;
        }

        VtValue value;
        std::string perr;
        if (!_ParseValueForAttribute(valueText, attr.GetTypeName(), &value, &perr)) {
            bad(pathStr + "." + attrName + ": " + perr); continue;
        }

        // Resolve effective time (item, then top-level, then Default).
        UsdTimeCode time = UsdTimeCode::Default();
        if (JsHasKey(item, "time"))      time = UsdTimeCode(JsGetDouble(item, "time", 0.0));
        else if (topHasTime)             time = UsdTimeCode(topTime);

        toApply.push_back({attr.GetPath(), value, time});
    }

    if (toApply.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toApply.size();
    UsdStageRefPtr stageCopy = stage;

    std::function<void()> fn = [stageCopy, items = std::move(toApply), ident]() {
        SdfLayerHandle l = SdfLayer::Find(ident);
        if (!l) return;
        UsdEditContext ctx(stageCopy, UsdEditTarget(l));
        for (const auto& it : items) {
            UsdAttribute a = stageCopy->GetAttributeAtPath(it.attrPath);
            if (a) a.Set(it.value, it.time);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: set " << okCount << " attribute"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_attribute_value to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 9. set_actives  (queued, batched)
//
// Top-level optional `active` default; per-item `path` required, optional
// `active` override. `layer_id` is top-level only so the batch lands in one
// undoable command. Accepts `items` or a `list_id`.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetActives(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    JsArray itemsArr;
    std::string itemsErr;
    if (!_ResolveBatchItems(args, itemsArr, itemsErr)) return itemsErr;

    // Shared default.
    const bool topHasActive = JsHasKey(args, "active");
    const bool topActive    = JsGetBool(args, "active", true);

    // Resolve target layer once.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;
    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = targetLayer ? _LayerName(targetLayer) : "<none>";
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

    struct Item { SdfPath path; bool active; };
    std::vector<Item>        toApply;
    std::vector<std::string> errors;
    toApply.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }
        UsdPrim prim = stage->GetPrimAtPath(primPath);
        if (!prim) { bad(pathStr + ": no prim at this path"); continue; }

        const bool itemHasActive = JsHasKey(item, "active");
        if (!(itemHasActive || topHasActive)) {
            bad(pathStr + ": missing 'active' (set per-item or top-level)");
            continue;
        }
        const bool active = itemHasActive ? JsGetBool(item, "active") : topActive;

        toApply.push_back({primPath, active});
    }

    if (toApply.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toApply.size();
    UsdStageRefPtr stageCopy = stage;

    std::function<void()> fn = [stageCopy, items = std::move(toApply), ident]() {
        SdfLayerHandle l = SdfLayer::Find(ident);
        if (!l) return;
        UsdEditContext ctx(stageCopy, UsdEditTarget(l));
        for (const auto& it : items) {
            // A prim may have been pruned by deactivating an ancestor earlier
            // in the batch — skip if it is no longer composed.
            UsdPrim p = stageCopy->GetPrimAtPath(it.path);
            if (p) p.SetActive(it.active);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: set active on " << okCount << " prim"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_prim_info to confirm.";
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
// 11. set_visibilities  (queued, batched — convenience wrapper around set_attributes)
//
// Top-level optional `visibility` default; per-item `path` required, optional
// `visibility` override. `layer_id` top-level only so the batch lands in a
// single undoable command.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetVisibilities(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    // Effective items come from either the explicit `items` array or, when
    // `list_id` is given, the named client-side list (one {path} per entry;
    // non-path fields come from the top-level shared defaults below).
    JsArray itemsArr;
    std::string itemsErr;
    if (!_ResolveBatchItems(args, itemsArr, itemsErr)) return itemsErr;

    // "visible" is accepted as an alias for "inherited" (USD's canonical name).
    auto normalize = [](const std::string& v) {
        return v == "visible" ? std::string("inherited") : v;
    };
    auto isValid = [](const std::string& v) {
        return v == "inherited" || v == "invisible" || v == "visible";
    };

    // Shared default.
    const bool        topHasVis = JsHasKey(args, "visibility");
    const std::string topVisRaw = JsGetString(args, "visibility");
    if (topHasVis && !topVisRaw.empty() && !isValid(topVisRaw)) {
        return "[error] top-level 'visibility' must be \"inherited\", "
               "\"invisible\", or \"visible\"; got \"" + topVisRaw + "\"";
    }
    const std::string topVis = normalize(topVisRaw);

    // Resolve target layer once.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;
    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = targetLayer ? _LayerName(targetLayer) : "<none>";
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

    struct Item { SdfPath attrPath; TfToken value; };
    std::vector<Item>        toApply;
    std::vector<std::string> errors;
    toApply.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }
        UsdPrim prim = stage->GetPrimAtPath(primPath);
        if (!prim) { bad(pathStr + ": no prim at this path"); continue; }

        UsdGeomImageable img(prim);
        if (!img) {
            bad(pathStr + ": not UsdGeomImageable"); continue;
        }
        UsdAttribute attr = img.GetVisibilityAttr();
        if (!attr) {
            bad(pathStr + ": no visibility attribute"); continue;
        }

        const bool        itemHasVis = JsHasKey(item, "visibility");
        const std::string visRaw     = itemHasVis
            ? JsGetString(item, "visibility")
            : topVisRaw;
        if (!(itemHasVis || topHasVis) || visRaw.empty()) {
            bad(pathStr + ": missing 'visibility' (set per-item or top-level)");
            continue;
        }
        if (!isValid(visRaw)) {
            bad(pathStr + ": 'visibility' must be \"inherited\", \"invisible\", "
                "or \"visible\"; got \"" + visRaw + "\""); continue;
        }

        toApply.push_back({attr.GetPath(), TfToken(normalize(visRaw))});
    }

    if (toApply.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toApply.size();
    UsdStageRefPtr stageCopy = stage;

    std::function<void()> fn = [stageCopy, items = std::move(toApply), ident]() {
        SdfLayerHandle l = SdfLayer::Find(ident);
        if (!l) return;
        UsdEditContext ctx(stageCopy, UsdEditTarget(l));
        for (const auto& it : items) {
            UsdAttribute a = stageCopy->GetAttributeAtPath(it.attrPath);
            if (a) a.Set(VtValue(it.value), UsdTimeCode::Default());
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: set visibility on " << okCount << " prim"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_attribute_value on visibility to confirm.";
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

    // Source the paths: either an explicit `paths` array or a stored `list_id`.
    std::vector<SdfPath> paths;
    const bool hasList  = JsHasKey(args, "list_id");
    const bool hasPaths = JsHasKey(args, "paths");
    if (hasList && hasPaths) {
        return "[error] use either 'list_id' or 'paths', not both";
    }
    if (hasList) {
        const std::string name = JsGetString(args, "list_id");
        if (!_GetListCopy(name, paths)) {
            return "[error] no list \"" + name + "\"; call manage_lists "
                   "operation=list to see stored lists";
        }
        // Stored list paths are already validated SdfPaths.
    } else {
        if (!hasPaths) {
            return "[error] missing 'paths' or 'list_id' (use paths=[] to clear "
                   "the selection)";
        }
        JsArray rawPaths = JsGetArray(args, "paths");
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
// 13. create_prims  (queued, batched)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::CreatePrims(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    if (!JsHasKey(args, "items"))
        return "[error] missing 'items' argument";
    const JsArray itemsArr = JsGetArray(args, "items");
    if (itemsArr.empty())
        return "[error] 'items' is empty — nothing to create";

    // Resolve target layer once for the whole batch.
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

    // Per-item validated form, ready to author.
    struct Item {
        SdfPath      path;
        SdfSpecifier specifier;
        std::string  typeName;
    };
    std::vector<Item>        toCreate;
    std::vector<std::string> errors;  // pre-formatted "  [i] msg" lines
    std::set<SdfPath>        seenInBatch;
    toCreate.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath path(pathStr);
        if (!path.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }
        if (!seenInBatch.insert(path).second) {
            bad(pathStr + ": duplicate path within batch"); continue;
        }
        if (targetLayer->GetPrimAtPath(path)) {
            bad(pathStr + ": a spec already exists at that path in "
                + targetLayerName); continue;
        }

        SdfSpecifier specifier = SdfSpecifierDef;
        const std::string specStr = JsGetString(item, "specifier");
        if (specStr == "over")       specifier = SdfSpecifierOver;
        else if (specStr == "class") specifier = SdfSpecifierClass;
        else if (!specStr.empty() && specStr != "def") {
            bad(pathStr + ": 'specifier' must be \"def\", \"over\", or \"class\""
                "; got \"" + specStr + "\""); continue;
        }

        toCreate.push_back({path, specifier, JsGetString(item, "type")});
    }

    if (toCreate.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toCreate.size();

    std::function<void()> fn = [items = std::move(toCreate), ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        for (const auto& it : items) {
            SdfPrimSpecHandle newSpec;
            if (it.path.IsRootPrimPath()) {
                newSpec = SdfPrimSpec::New(layer, it.path.GetName(), it.specifier);
                layer->InsertRootPrim(newSpec);
            } else {
                SdfPrimSpecHandle parent =
                    _EnsurePrimSpec(layer, it.path.GetParentPath());
                if (!parent) continue;
                newSpec = SdfPrimSpec::New(parent, it.path.GetName(), it.specifier);
            }
            if (newSpec && !it.typeName.empty()) newSpec->SetTypeName(it.typeName);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: create " << okCount << " prim"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with list_children to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// Composition arc helpers — shared by add_references, add_payloads,
// add_inherits, add_specializes.
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

// Batch helper for path-arc tools (add_inherits, add_specializes). They share
// the same shape (path + target_path per item) and only differ in which list
// proxy the appender writes to. `label` is the user-facing arc noun (e.g.
// "inherit arc") used in the result string.
using _PathArcAppender =
    std::function<void(const SdfPrimSpecHandle&, const SdfPath&)>;

std::string _AddPathArcsBatch(const UsdStageRefPtr& stage,
                              const JsObject&       args,
                              const std::string&    label,
                              _PathArcAppender      appender) {
    if (!stage) return "[error] no active stage";

    if (!JsHasKey(args, "items"))
        return "[error] missing 'items' argument";
    const JsArray itemsArr = JsGetArray(args, "items");
    if (itemsArr.empty())
        return "[error] 'items' is empty — nothing to add";

    std::string targetLayerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, targetLayerName, err);
    if (!targetLayer) return err;

    struct Item { SdfPath primPath; SdfPath targetPath; };
    std::vector<Item>        toAdd;
    std::vector<std::string> errors;
    toAdd.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }

        const std::string targetStr = JsGetString(item, "target_path");
        if (targetStr.empty()) {
            bad(pathStr + ": missing 'target_path'"); continue;
        }
        if (!SdfPath::IsValidPathString(targetStr)) {
            bad(pathStr + ": 'target_path' is not a valid SdfPath: " + targetStr);
            continue;
        }
        SdfPath targetPath(targetStr);
        if (!targetPath.IsAbsolutePath()) {
            bad(pathStr + ": 'target_path' must be an absolute path"); continue;
        }

        toAdd.push_back({primPath, targetPath});
    }

    if (toAdd.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toAdd.size();

    std::function<void()> fn = [items = std::move(toAdd), ident,
                                appender = std::move(appender)]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        for (const auto& it : items) {
            SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, it.primPath);
            if (spec) appender(spec, it.targetPath);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add " << okCount << " " << label
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

} // anonymous namespace

// --------------------------------------------------------------------------
// 14. add_references  (queued, batched)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddReferences(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    if (!JsHasKey(args, "items"))
        return "[error] missing 'items' argument";
    const JsArray itemsArr = JsGetArray(args, "items");
    if (itemsArr.empty())
        return "[error] 'items' is empty — nothing to add";

    std::string targetLayerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, targetLayerName, err);
    if (!targetLayer) return err;

    struct Item {
        SdfPath      primPath;
        SdfReference ref;
    };
    std::vector<Item>        toAdd;
    std::vector<std::string> errors;
    toAdd.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }

        if (!JsHasKey(item, "asset_path")) {
            bad(pathStr + ": missing 'asset_path' (use \"\" for an internal "
                "reference)"); continue;
        }
        const std::string assetPath = JsGetString(item, "asset_path");

        SdfPath targetPrimPath;
        const std::string targetStr = JsGetString(item, "prim_path");
        if (!targetStr.empty()) {
            if (!SdfPath::IsValidPathString(targetStr)) {
                bad(pathStr + ": 'prim_path' is not a valid SdfPath: " + targetStr);
                continue;
            }
            targetPrimPath = SdfPath(targetStr);
        }

        const double offset = JsGetDouble(item, "layer_offset", 0.0);
        const double scale  = JsGetDouble(item, "layer_scale",  1.0);

        toAdd.push_back({primPath,
                         SdfReference(assetPath, targetPrimPath,
                                      SdfLayerOffset(offset, scale))});
    }

    if (toAdd.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toAdd.size();

    std::function<void()> fn = [items = std::move(toAdd), ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        for (const auto& it : items) {
            SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, it.primPath);
            if (spec) spec->GetReferenceList().Prepend(it.ref);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add " << okCount << " reference"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 15. add_payloads  (queued, batched)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddPayloads(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    if (!JsHasKey(args, "items"))
        return "[error] missing 'items' argument";
    const JsArray itemsArr = JsGetArray(args, "items");
    if (itemsArr.empty())
        return "[error] 'items' is empty — nothing to add";

    std::string targetLayerName, err;
    SdfLayerHandle targetLayer = _ResolveArcLayer(stage, args, targetLayerName, err);
    if (!targetLayer) return err;

    struct Item {
        SdfPath    primPath;
        SdfPayload payload;
    };
    std::vector<Item>        toAdd;
    std::vector<std::string> errors;
    toAdd.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }

        if (!JsHasKey(item, "asset_path")) {
            bad(pathStr + ": missing 'asset_path'"); continue;
        }
        const std::string assetPath = JsGetString(item, "asset_path");

        SdfPath targetPrimPath;
        const std::string targetStr = JsGetString(item, "prim_path");
        if (!targetStr.empty()) {
            if (!SdfPath::IsValidPathString(targetStr)) {
                bad(pathStr + ": 'prim_path' is not a valid SdfPath: " + targetStr);
                continue;
            }
            targetPrimPath = SdfPath(targetStr);
        }

        const double offset = JsGetDouble(item, "layer_offset", 0.0);
        const double scale  = JsGetDouble(item, "layer_scale",  1.0);

        toAdd.push_back({primPath,
                         SdfPayload(assetPath, targetPrimPath,
                                    SdfLayerOffset(offset, scale))});
    }

    if (toAdd.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toAdd.size();

    std::function<void()> fn = [items = std::move(toAdd), ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        for (const auto& it : items) {
            SdfPrimSpecHandle spec = _EnsurePrimSpec(layer, it.primPath);
            if (spec) spec->GetPayloadList().Prepend(it.payload);
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: add " << okCount << " payload"
        << (okCount == 1 ? "" : "s") << " on layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Re-read with get_composition_arcs to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// 16. add_inherits  (queued, batched)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddInherits(const JsObject& args) const {
    return _AddPathArcsBatch(_stageFn(), args, "inherit arc",
        [](const SdfPrimSpecHandle& s, const SdfPath& t) {
            s->GetInheritPathList().Prepend(t);
        });
}

// --------------------------------------------------------------------------
// 17. add_specializes  (queued, batched)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::AddSpecializes(const JsObject& args) const {
    return _AddPathArcsBatch(_stageFn(), args, "specialize arc",
        [](const SdfPrimSpecHandle& s, const SdfPath& t) {
            s->GetSpecializesList().Prepend(t);
        });
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
// 21. delete_prims  (queued, batched)
//
// Removes the SdfPrimSpec for each path from a single target layer in one
// undoable command. `path` is per-item; `layer_id` is top-level only so the
// whole batch lands in one undo entry. Accepts `items` or a `list_id`.
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::DeletePrims(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    JsArray itemsArr;
    std::string itemsErr;
    if (!_ResolveBatchItems(args, itemsArr, itemsErr)) return itemsErr;

    // Resolve target layer once — named via layer_id or current edit target.
    const std::string layerIdArg = JsGetString(args, "layer_id");
    SdfLayerHandle targetLayer;
    std::string targetLayerName;
    if (layerIdArg.empty()) {
        targetLayer = stage->GetEditTarget().GetLayer();
        targetLayerName = targetLayer ? _LayerName(targetLayer) : "<none>";
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

    std::vector<SdfPath>     toDelete;
    std::vector<std::string> errors;
    toDelete.reserve(itemsArr.size());

    for (size_t i = 0; i < itemsArr.size(); ++i) {
        auto bad = [&](const std::string& msg) {
            errors.push_back("  [" + std::to_string(i) + "] " + msg);
        };
        const JsValue& v = itemsArr[i];
        if (!v.IsObject()) { bad("item is not an object"); continue; }
        const JsObject& item = v.GetJsObject();

        const std::string pathStr = JsGetString(item, "path");
        if (pathStr.empty()) { bad("missing 'path'"); continue; }
        if (!SdfPath::IsValidPathString(pathStr)) {
            bad(pathStr + ": not a valid SdfPath"); continue;
        }
        SdfPath primPath(pathStr);
        if (!primPath.IsPrimPath()) {
            bad(pathStr + ": not a prim path"); continue;
        }
        if (!targetLayer->GetPrimAtPath(primPath)) {
            bad(pathStr + ": no spec in " + targetLayerName
                + " (the prim may be defined in another layer — use layer_id)");
            continue;
        }
        toDelete.push_back(primPath);
    }

    if (toDelete.empty()) {
        std::ostringstream oss;
        oss << "[error] no valid items in batch (" << itemsArr.size()
            << " requested, " << errors.size() << " failed):";
        for (const auto& e : errors) oss << "\n" << e;
        return oss.str();
    }

    const std::string ident = targetLayer->GetIdentifier();
    const size_t okCount = toDelete.size();

    std::function<void()> fn = [paths = std::move(toDelete), ident]() {
        SdfLayerHandle layer = SdfLayer::Find(ident);
        if (!layer) return;
        for (const SdfPath& path : paths) {
            // A spec may already be gone if an ancestor in the same batch was
            // removed first — that's fine, the end state is identical.
            SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
            if (!spec) continue;
            if (SdfPrimSpecHandle parent = spec->GetNameParent()) {
                parent->RemoveNameChild(spec);
            } else {
                layer->RemoveRootPrim(spec);
            }
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(targetLayer, fn);

    std::ostringstream oss;
    oss << "Queued: delete " << okCount << " spec" << (okCount == 1 ? "" : "s")
        << " from layer " << targetLayerName;
    if (!errors.empty()) {
        oss << ". " << okCount << " ok, " << errors.size() << " errors:";
        for (const auto& e : errors) oss << "\n" << e;
    } else {
        oss << ".";
    }
    oss << " Note: if other layers hold opinions on a prim it will still appear "
           "on the composed stage. Re-read with get_prim_info to confirm.";
    return oss.str();
}

// --------------------------------------------------------------------------
// find_usd_files  — helpers and implementation
// --------------------------------------------------------------------------
namespace {

bool _IsUsdExt(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}

// Only .usda and .usd may be text; .usdc is binary Crate, .usdz is a zip.
bool _IsGrepableExt(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".usd" || ext == ".usda";
}

// Returns false if the file looks binary (null byte in first 1 KB).
bool _IsTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char buf[1024];
    f.read(buf, sizeof(buf));
    const std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i)
        if (buf[i] == '\0') return false;
    return true;
}

struct FileGrepMatch {
    std::string              path;
    std::vector<std::string> lines;  // up to 3 "  line N: <text>"
    int                      total = 0;
};

FileGrepMatch _GrepFile(const std::string& path, const std::string& patLower) {
    FileGrepMatch m;
    m.path = path;
    std::ifstream f(path);
    if (!f) return m;
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        ++n;
        if (_ToLower(line).find(patLower) != std::string::npos) {
            ++m.total;
            if ((int)m.lines.size() < 3) {
                std::string disp = line.size() > 120
                    ? line.substr(0, 120) + "..." : line;
                m.lines.push_back("  line " + std::to_string(n) + ": " + disp);
            }
        }
    }
    return m;
}

} // anonymous namespace

std::string UsdToolDispatcher::FindUsdFiles(const JsObject& args) const {
    namespace fs = std::filesystem;

    const std::string namePat    = JsGetString(args, "name_pattern");
    const std::string contentPat = JsGetString(args, "content_pattern");
    const bool        recursive  = JsGetBool(args, "recursive", true);

    if (namePat.empty() && contentPat.empty())
        return "[error] provide at least one of 'name_pattern' or 'content_pattern'";

    // Resolve search directories.
    std::vector<std::string> dirs;
    if (JsHasKey(args, "directories")) {
        for (const JsValue& v : JsGetArray(args, "directories"))
            if (v.IsString()) dirs.push_back(v.GetString());
    }
    if (dirs.empty()) {
        // Fall back to the parent directory of the current stage's root layer.
        UsdStageRefPtr stage = _stageFn ? _stageFn() : UsdStageRefPtr();
        if (stage) {
            SdfLayerHandle root = stage->GetRootLayer();
            if (root && !root->IsAnonymous()) {
                const std::string rp = root->GetRealPath();
                if (!rp.empty())
                    dirs.push_back(fs::path(rp).parent_path().string());
            }
        }
        if (dirs.empty())
            return "[error] no directories to search — specify 'directories' "
                   "or open an on-disk USD stage first.";
    }

    // Validate.
    for (const std::string& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec))
            return "[error] not a directory: '" + d + "'";
    }

    const std::string nameLower    = _ToLower(namePat);
    const std::string contentLower = _ToLower(contentPat);

    // Phase 1: walk every directory on its own thread — no cap on what is
    // collected; we cap what is *displayed*, not what is searched.
    std::vector<std::future<std::vector<std::string>>> walkFutures;
    walkFutures.reserve(dirs.size());

    for (const std::string& dir : dirs) {
        walkFutures.push_back(std::async(std::launch::async,
            [dir, nameLower, recursive]() -> std::vector<std::string> {
                std::vector<std::string> found;
                try {
                    std::error_code ec;
                    auto visit = [&](const std::filesystem::path& p) {
                        if (!_IsUsdExt(p)) return;
                        if (!nameLower.empty()) {
                            if (_ToLower(p.filename().string()).find(nameLower)
                                    == std::string::npos) return;
                        }
                        found.push_back(p.string());
                    };
                    using opts = std::filesystem::directory_options;
                    if (recursive) {
                        for (const auto& e : std::filesystem::recursive_directory_iterator(
                                 dir, opts::skip_permission_denied, ec))
                            if (e.is_regular_file(ec)) visit(e.path());
                    } else {
                        for (const auto& e : std::filesystem::directory_iterator(
                                 dir, opts::skip_permission_denied, ec))
                            if (e.is_regular_file(ec)) visit(e.path());
                    }
                } catch (...) {}
                return found;
            }));
    }

    std::vector<std::string> allFiles;
    for (auto& fut : walkFutures)
        for (std::string& p : fut.get())
            allFiles.push_back(std::move(p));

    if (allFiles.empty()) {
        std::ostringstream oss;
        oss << "No USD files";
        if (!namePat.empty()) oss << " with name matching '" << namePat << "'";
        oss << " found in:";
        for (const auto& d : dirs) oss << "\n  " << d;
        return oss.str();
    }

    // Phase 2: content grep — batched by hardware_concurrency to bound the
    // number of live threads; all files are searched, results are capped.
    if (!contentPat.empty()) {
        // Collect grepable candidates.
        std::vector<std::string> toGrep;
        for (const auto& fp : allFiles)
            if (_IsGrepableExt(fs::path(fp))) toGrep.push_back(fp);

        const int batchSize = std::max(4,
            static_cast<int>(std::thread::hardware_concurrency()));

        std::vector<FileGrepMatch> matches;
        for (size_t i = 0; i < toGrep.size(); i += batchSize) {
            const size_t end = std::min(i + static_cast<size_t>(batchSize),
                                        toGrep.size());
            std::vector<std::future<FileGrepMatch>> batch;
            batch.reserve(end - i);
            for (size_t j = i; j < end; ++j) {
                const std::string fp = toGrep[j];
                batch.push_back(std::async(std::launch::async,
                    [fp, contentLower]() -> FileGrepMatch {
                        if (!_IsTextFile(fp)) return {};
                        return _GrepFile(fp, contentLower);
                    }));
            }
            for (auto& f : batch) {
                FileGrepMatch m = f.get();
                if (m.total > 0) matches.push_back(std::move(m));
            }
        }

        static constexpr int kMaxMatchDisplay = 50;
        const int display = std::min((int)matches.size(), kMaxMatchDisplay);
        std::ostringstream oss;
        for (int i = 0; i < display; ++i) {
            const FileGrepMatch& m = matches[i];
            oss << m.path << "\n";
            for (const auto& l : m.lines) oss << l << "\n";
            const int remaining = m.total - (int)m.lines.size();
            if (remaining > 0)
                oss << "  (... " << remaining << " more match"
                    << (remaining == 1 ? "" : "es") << ")\n";
        }
        if (matches.empty()) {
            oss << "No files contain '" << contentPat << "'";
            if (!namePat.empty())
                oss << " (searched " << toGrep.size()
                    << " files matching '" << namePat << "')";
        } else {
            oss << matches.size() << " file" << (matches.size() == 1 ? "" : "s")
                << " of " << toGrep.size() << " searched contain '"
                << contentPat << "'";
            if ((int)matches.size() > kMaxMatchDisplay)
                oss << " (showing first " << kMaxMatchDisplay << ")";
        }
        oss << "\n";
        return oss.str();
    }

    // Name-only results — cap display, report total.
    static constexpr int kMaxNameDisplay = 200;
    const int display = std::min((int)allFiles.size(), kMaxNameDisplay);
    std::ostringstream oss;
    oss << allFiles.size() << " USD file"
        << (allFiles.size() == 1 ? "" : "s")
        << " matching '" << namePat << "'";
    if ((int)allFiles.size() > kMaxNameDisplay)
        oss << " (showing first " << kMaxNameDisplay << ")";
    oss << ":\n";
    for (int i = 0; i < display; ++i) oss << "  " << allFiles[i] << "\n";
    return oss.str();
}

// --------------------------------------------------------------------------
// open_file  (queued on UI thread)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::OpenFile(const JsObject& args) const {
    const std::string path = JsGetString(args, "path");
    if (path.empty()) return "[error] missing 'path' argument";

    if (!_openFileFn)
        return "[error] open_file is not available in this context";

    const std::string modeStr = JsGetString(args, "mode");
    bool asStage = true;
    if (modeStr == "layer") {
        asStage = false;
    } else if (!modeStr.empty() && modeStr != "stage") {
        return "[error] 'mode' must be \"stage\" or \"layer\"; got \""
               + modeStr + "\"";
    }

    OpenFileProvider fn = _openFileFn;
    QueueOnUIThread([fn, path, asStage]() { fn(path, asStage); });

    std::ostringstream oss;
    oss << "Queued: open \"" << path << "\" as " << (asStage ? "stage" : "layer")
        << ". The active scene will change on the next frame — "
           "call get_stage_info on your next step to confirm the new stage.";
    return oss.str();
}

// --------------------------------------------------------------------------
// create_layer_file  — create a new empty USD layer file on disk
// --------------------------------------------------------------------------
// Materialises a sublayer target that does not exist yet. Creation is
// SYNCHRONOUS (not queued via ExecuteAfterDraw) so the model gets an accurate
// success/failure and the real on-disk path back in the same step — the file
// must exist before a queued add_sublayer recomposes the stage on the next
// frame. SdfLayer::CreateNew is registry-safe and does not touch the active
// stage (nothing references the new layer yet), so it is safe to call on the
// worker thread alongside the read-only tools that already traverse the stage.
//
// Permission is conversational: Twiki must ask the user before calling this
// (enforced by the system prompt + this tool's description), not by a host
// modal — the orchestrator runs on a background thread with no UI gate.
std::string UsdToolDispatcher::CreateLayerFile(const JsObject& args) const {
    namespace fs = std::filesystem;

    const std::string pathArg = JsGetString(args, "path");
    if (pathArg.empty()) return "[error] missing 'path' argument";

    // Must be a USD layer extension SdfLayer can write (not .usdz — that is a
    // package, not a writable layer).
    fs::path p(pathArg);
    {
        std::string ext = p.extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".usd" && ext != ".usda" && ext != ".usdc")
            return "[error] path must end in .usd, .usda or .usdc; got \""
                 + pathArg + "\"";
    }

    // Resolve a relative path against the current stage's root-layer directory,
    // so the model can pass the same string it gives add_sublayer (sublayer
    // paths are stored relative to the layer that owns them).
    std::string resolved = pathArg;
    if (p.is_relative()) {
        UsdStageRefPtr stage = _stageFn ? _stageFn() : UsdStageRefPtr();
        SdfLayerHandle root = stage ? stage->GetRootLayer() : SdfLayerHandle();
        if (root && !root->IsAnonymous()) {
            const std::string rp = root->GetRealPath();
            if (!rp.empty())
                resolved = (fs::path(rp).parent_path() / p).string();
        } else {
            return "[error] cannot resolve relative path \"" + pathArg
                 + "\" — the current stage has no on-disk root layer. "
                   "Pass an absolute path.";
        }
    }

    // Never clobber an existing file — if it exists the model should just
    // add_sublayer directly.
    std::error_code ec;
    if (fs::exists(resolved, ec))
        return "[error] file already exists: \"" + resolved
             + "\". No need to create it — add it as a sublayer directly.";

    // Make sure the parent directory exists.
    const fs::path parent = fs::path(resolved).parent_path();
    if (!parent.empty() && !fs::exists(parent, ec)) {
        fs::create_directories(parent, ec);
        if (ec)
            return "[error] could not create parent directory \""
                 + parent.string() + "\": " + ec.message();
    }

    SdfLayerRefPtr layer = SdfLayer::CreateNew(resolved);
    if (!layer)
        return "[error] SdfLayer::CreateNew failed for \"" + resolved
             + "\" — check the path is writable and the extension is supported.";
    layer->Save();

    std::ostringstream oss;
    oss << "Created empty USD layer \"" << resolved
        << "\". You can now add it as a sublayer with add_sublayer.";
    return oss.str();
}

// --------------------------------------------------------------------------
// get_edit_target
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::GetEditTarget(const JsObject& /*args*/) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    SdfLayerHandle layer = stage->GetEditTarget().GetLayer();
    if (!layer) return "[error] no edit target set";

    std::ostringstream oss;
    oss << "edit target: " << _LayerName(layer);
    if (!layer->PermissionToEdit()) oss << "  [readonly]";
    if (stage->IsLayerMuted(layer->GetIdentifier())) oss << "  [muted]";
    oss << "\nidentifier: " << layer->GetIdentifier();
    return oss.str();
}

// --------------------------------------------------------------------------
// set_edit_target  (queued on UI thread — stage->SetEditTarget is not
// an SDF mutation so it can't use ExecuteAfterDraw<UsdFunctionCall>)
// --------------------------------------------------------------------------
std::string UsdToolDispatcher::SetEditTarget(const JsObject& args) const {
    UsdStageRefPtr stage = _stageFn();
    if (!stage) return "[error] no active stage";

    const std::string layerId = JsGetString(args, "layer_id");
    if (layerId.empty()) return "[error] missing 'layer_id' argument";

    SdfLayerHandle layer = _FindLayer(stage, layerId);
    if (!layer)
        return "[error] layer '" + layerId
               + "' not found in the layer stack. "
                 "Call get_layer_stack to see available layers.";

    if (!layer->PermissionToEdit())
        return "[error] layer '" + _LayerName(layer) + "' is read-only";
    if (stage->IsLayerMuted(layer->GetIdentifier()))
        return "[error] layer '" + _LayerName(layer) + "' is muted";

    QueueOnUIThread([stage, layer]() {
        stage->SetEditTarget(UsdEditTarget(layer));
    });

    return "Queued: set edit target to " + _LayerName(layer)
           + ". Re-read with get_edit_target on your next step to confirm.";
}

} // namespace UsdAgent
