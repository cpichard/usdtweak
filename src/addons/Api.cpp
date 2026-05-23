#include "Api.h"

#include "Editor.h"
#include "EditorSettings.h"
#include "Selection.h"
#include "search/StringSearchIndex.h"
#include "viewport/Viewport.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdtweak {

namespace {
Editor *gEditor = nullptr;
}

void _RegisterEditor(Editor *editor) { gEditor = editor; }

Editor *GetEditor() { return gEditor; }

// ---------------------------------------------------------------- state reads
UsdStageRefPtr GetCurrentStage() { return gEditor ? gEditor->GetCurrentStage() : UsdStageRefPtr(); }
SdfLayerRefPtr GetCurrentLayer() { return gEditor ? gEditor->GetCurrentLayer() : SdfLayerRefPtr(); }

UsdEditTarget GetCurrentEditTarget() {
    auto stage = GetCurrentStage();
    return stage ? stage->GetEditTarget() : UsdEditTarget();
}

UsdStageCache &GetStageCache() { return gEditor->GetStageCache(); }

UsdTimeCode GetCurrentTimeCode() {
    return gEditor ? gEditor->GetViewport().GetCurrentTimeCode() : UsdTimeCode::Default();
}

const Selection &GetSelection() { return gEditor->GetSelection(); }

// ---------------------------------------------------------- state mutations
void SetCurrentStage(UsdStageRefPtr stage) {
    if (gEditor) gEditor->SetCurrentStage(stage);
}
void SetCurrentLayer(SdfLayerRefPtr layer) {
    if (gEditor) gEditor->SetCurrentLayer(layer);
}
void SetCurrentEditTarget(SdfLayerHandle layer) {
    if (gEditor) gEditor->SetCurrentEditTarget(layer);
}

void SetStagePathSelection(const SdfPath &primPath) {
    if (gEditor) gEditor->SetStagePathSelection(primPath);
}
void AddStagePathSelection(const SdfPath &primPath) {
    if (gEditor) gEditor->AddStagePathSelection(primPath);
}
void SetLayerPathSelection(const SdfPath &primPath) {
    if (gEditor) gEditor->SetLayerPathSelection(primPath);
}
void AddLayerPathSelection(const SdfPath &primPath) {
    if (gEditor) gEditor->AddLayerPathSelection(primPath);
}

void OpenStage(const std::string &path) {
    if (gEditor) gEditor->OpenStage(path);
}
void FindOrOpenLayer(const std::string &path) {
    if (gEditor) gEditor->FindOrOpenLayer(path);
}
void CreateStage(const std::string &path) {
    if (gEditor) gEditor->CreateStage(path);
}

void FrameCameraOnSelection() {
    if (gEditor) gEditor->GetViewport().FrameCameraOnSelection(gEditor->GetSelection());
}

// ----------------------------------------------------------- search service
std::vector<SdfPath> SearchPrimsByName(const std::string &needle, uint32_t limit) {
    const uint32_t mask = static_cast<uint32_t>(SearchCategory::PrimName) |
                          static_cast<uint32_t>(SearchCategory::UsdPrimName);
    const auto results = StringSearchIndex::GetInstance().Query(needle, mask);
    std::vector<SdfPath> paths;
    for (const auto &r : results) {
        for (const auto &p : r.paths) {
            paths.push_back(p);
            if (limit && paths.size() >= limit) return paths;
        }
    }
    return paths;
}

// --------------------------------------------------------- settings access
static EditorSettings *_Settings() { return gEditor ? &gEditor->GetSettingsForAddons() : nullptr; }

bool GetAddonBool(const std::string &addonId, const std::string &key, bool defaultValue) {
    auto *s = _Settings();
    return s ? s->GetAddonBool(addonId, key, defaultValue) : defaultValue;
}
void SetAddonBool(const std::string &addonId, const std::string &key, bool value) {
    if (auto *s = _Settings()) s->SetAddonBool(addonId, key, value);
}
std::string GetAddonString(const std::string &addonId, const std::string &key, const std::string &defaultValue) {
    auto *s = _Settings();
    return s ? s->GetAddonString(addonId, key, defaultValue) : defaultValue;
}
void SetAddonString(const std::string &addonId, const std::string &key, const std::string &value) {
    if (auto *s = _Settings()) s->SetAddonString(addonId, key, value);
}

} // namespace usdtweak
