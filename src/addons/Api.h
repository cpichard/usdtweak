#pragma once

///
/// Frontend API for usdtweak addons.
///
/// One include — pulls in everything a typical addon needs:
///   - addon registration (AddonRegistry.h + TF_REGISTRY_FUNCTION_WITH_TAG)
///   - editor state accessors (current stage, layer, edit target, selection)
///   - undoable mutations via ExecuteAfterDraw<>
///   - modal dialog launcher
///   - editor notices to subscribe to
///
/// Example:
///   #include "addons/Api.h"
///   TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, MyAddon) {
///       UsdTweakAddonRegistry::GetInstance().Add({
///           .id = "MyAddon", .menuLabel = "My Addon",
///           .kind = UsdTweakAddon::Kind::Window,
///           .draw = []{ ImGui::Text("hello"); },
///       });
///   }
///

#include <pxr/base/tf/registryManager.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include "AddonRegistry.h"
#include "Notices.h"
#include "commands/Commands.h"     // ExecuteAfterDraw<>
#include "widgets/ModalDialogs.h"  // DrawModalDialog<>

PXR_NAMESPACE_USING_DIRECTIVE

class Editor;
struct Selection;
class Viewport;

namespace usdtweak {

/// Editor escape hatch. Use only when the curated accessors below are
/// insufficient; coupling to Editor is what the rest of this API is trying
/// to avoid.
Editor *GetEditor();

// ---------------------------------------------------------------- state reads
UsdStageRefPtr GetCurrentStage();
SdfLayerRefPtr GetCurrentLayer();
UsdEditTarget GetCurrentEditTarget();
UsdStageCache &GetStageCache();
UsdTimeCode GetCurrentTimeCode();

const Selection &GetSelection();

// ---------------------------------------------------------- state mutations
// All mutations route through commands → undoable.
void SetCurrentStage(UsdStageRefPtr stage);
void SetCurrentLayer(SdfLayerRefPtr layer);
void SetCurrentEditTarget(SdfLayerHandle layer);

void SetStagePathSelection(const SdfPath &primPath);
void AddStagePathSelection(const SdfPath &primPath);
void SetLayerPathSelection(const SdfPath &primPath);
void AddLayerPathSelection(const SdfPath &primPath);

void OpenStage(const std::string &path);
void FindOrOpenLayer(const std::string &path);
void CreateStage(const std::string &path);

void FrameCameraOnSelection();

// ----------------------------------------------------------- search service
/// Substring search over indexed prim paths. Returns at most `limit` matches.
/// Calls into src/search/StringSearchIndex; see that header for category bits.
std::vector<SdfPath> SearchPrimsByName(const std::string &needle, uint32_t limit = 0);

// --------------------------------------------------------- settings access
/// Per-addon persisted key/value bag, written into the same .ini file as
/// EditorSettings under the key prefix "Addon.<addonId>.<key>".
bool GetAddonBool(const std::string &addonId, const std::string &key, bool defaultValue = false);
void SetAddonBool(const std::string &addonId, const std::string &key, bool value);
std::string GetAddonString(const std::string &addonId, const std::string &key, const std::string &defaultValue = "");
void SetAddonString(const std::string &addonId, const std::string &key, const std::string &value);

// ---------------------------------------------------------------- internal
// Called by Editor on construction / destruction. Not for addon use.
void _RegisterEditor(Editor *editor);

} // namespace usdtweak
