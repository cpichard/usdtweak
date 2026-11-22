#include <functional>
#include <string>
#include <vector>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdRender/tokens.h>

#include "Commands.h"
#include "Constants.h"
#include "ImGuiHelpers.h"
#include "StageMetadataEditor.h"
#include "TableLayouts.h"
#include "VtDictionaryEditor.h"
#include "VtValueEditor.h"
#include <imgui_stdlib.h>

namespace {

/// The layer the stage reads a metadata field from
enum class StageMetadataSource { Fallback, RootLayer, SessionLayer };

/// One stage metadata row: how the stage reads the value and how to edit it
struct StageMetadataField {
    const char *label;
    TfToken key;
    TfToken deprecatedKey; // startFrame and endFrame, still read by the stage when the time codes aren't authored
    std::function<VtValue(const UsdStageWeakPtr &)> getValue;
    std::function<VtValue(const VtValue &)> drawEditor; // Returns the edited value, empty when not edited
};

bool LayerHasField(const SdfLayerHandle &layer, const TfToken &key) {
    return layer && !key.IsEmpty() && layer->HasField(SdfPath::AbsoluteRootPath(), key);
}

bool LayerHasField(const SdfLayerHandle &layer, const StageMetadataField &field) {
    return LayerHasField(layer, field.key) || LayerHasField(layer, field.deprecatedKey);
}

StageMetadataSource GetStageMetadataSource(const UsdStageWeakPtr &stage, const StageMetadataField &field) {
    if (LayerHasField(stage->GetSessionLayer(), field)) {
        return StageMetadataSource::SessionLayer;
    }
    if (LayerHasField(stage->GetRootLayer(), field)) {
        return StageMetadataSource::RootLayer;
    }
    return StageMetadataSource::Fallback;
}

std::string GetSourceText(StageMetadataSource source) {
    switch (source) {
    case StageMetadataSource::SessionLayer:
        return "Set in the session layer";
    case StageMetadataSource::RootLayer:
        return "Set in the root layer";
    default:
        return "Not authored, USD uses its fallback value";
    }
}

/// Why an edit in the edit target layer has no visible effect, empty when it has one
std::string GetShadowedReason(const UsdStageWeakPtr &stage, StageMetadataSource source) {
    if (source == StageMetadataSource::SessionLayer && stage->GetEditTarget().GetLayer() == stage->GetRootLayer()) {
        return "The session layer sets this value and wins over the root layer: an edit here has no visible effect.\n"
               "Choose the session layer as edit target to change it, or clear it there.";
    }
    return {};
}

void SetEditLayerField(const UsdStageWeakPtr &stage, const TfToken &key, const VtValue &value) {
    const SdfLayerRefPtr editLayer = stage->GetEditTarget().GetLayer();
    ExecuteAfterDraw(&SdfLayer::SetField<VtValue>, editLayer, SdfPath::AbsoluteRootPath(), key, value);
}

SdfLayerHandle GetSourceLayer(const UsdStageWeakPtr &stage, StageMetadataSource source) {
    switch (source) {
    case StageMetadataSource::SessionLayer:
        return stage->GetSessionLayer();
    case StageMetadataSource::RootLayer:
        return stage->GetRootLayer();
    default:
        return {};
    }
}

void ClearLayerFields(const SdfLayerHandle &layer, const TfToken &key, const TfToken &deprecatedKey) {
    const SdfLayerRefPtr layerRef = layer;
    for (const TfToken &field : {key, deprecatedKey}) {
        if (LayerHasField(layer, field)) {
            ExecuteAfterDraw(&SdfLayer::EraseField, layerRef, SdfPath::AbsoluteRootPath(), field);
        }
    }
}

/// "(m)" button like the "(a)" of the attributes: green when the edit target layer has an opinion, grey otherwise.
/// Its menu clears the opinion of the edit target layer or switches the edit target to the layer giving the value.
/// The layer is the one edited from this row: the edit target, or the root layer for the default prim.
void DrawMetadataMiniButton(const UsdStageWeakPtr &stage, const SdfLayerHandle &layer, const TfToken &key,
                            const TfToken &deprecatedKey, StageMetadataSource source) {
    const bool authoredHere = layer == stage->GetEditTarget().GetLayer() &&
                              (LayerHasField(layer, key) || LayerHasField(layer, deprecatedKey));
    {
        ScopedStyleColor miniButtonStyle(ImGuiCol_Text,
                                         authoredHere ? ImVec4(ColorMiniButtonAuthored) : ImVec4(ColorMiniButtonUnauthored),
                                         ImGuiCol_Button, ImVec4(ColorTransparent));
        ImGui::SmallButton("(m)");
    }
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        ScopedStyleColor defaultStyle(DefaultColorStyle);
        if (ImGui::MenuItem(ICON_FA_ERASER " Clear value", nullptr, false, authoredHere)) {
            ClearLayerFields(layer, key, deprecatedKey);
        }
        const SdfLayerHandle sourceLayer = GetSourceLayer(stage, source);
        if (ImGui::MenuItem("Edit strongest opinion", nullptr, false,
                            sourceLayer && sourceLayer != stage->GetEditTarget().GetLayer())) {
            ExecuteAfterDraw<EditorSetEditTarget>(stage, UsdEditTarget(sourceLayer));
        }
        ImGui::EndPopup();
    }
}

/// Like the attributes: the value set in a layer, a value hidden by the session layer, or the fallback in grey
ImVec4 GetValueColor(StageMetadataSource source, const std::string &shadowedReason) {
    if (!shadowedReason.empty()) {
        return ImVec4(ColorAttributeShadowed);
    }
    return source == StageMetadataSource::Fallback ? ImVec4(ColorAttributeUnauthored) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

void DrawValueTooltip(const std::string &text) {
    if (!text.empty() && IsItemHoveredForHint(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text.c_str());
    }
}

void DrawLabel(const char *label, bool authored) {
    ImGui::TextColored(authored ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored), "%s", label);
}

void DrawFieldRow(int &rowId, const UsdStageWeakPtr &stage, const StageMetadataField &field) {
    const StageMetadataSource source = GetStageMetadataSource(stage, field);
    const std::string shadowedReason = GetShadowedReason(stage, source);
    ImGui::PushID(rowId++);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
    ImGui::TableSetColumnIndex(0);
    DrawMetadataMiniButton(stage, stage->GetEditTarget().GetLayer(), field.key, field.deprecatedKey, source);
    ImGui::TableSetColumnIndex(1);
    DrawLabel(field.label, source != StageMetadataSource::Fallback);
    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    {
        ScopedStyleColor valueColor(ImGuiCol_Text, GetValueColor(source, shadowedReason));
        const VtValue edited = field.drawEditor(field.getValue(stage));
        if (!edited.IsEmpty()) {
            SetEditLayerField(stage, field.key, edited);
        }
    }
    DrawValueTooltip(shadowedReason.empty() ? GetSourceText(source) : GetSourceText(source) + "\n" + shadowedReason);
    ImGui::PopID();
}

//
// Default prim: USD reads it from the root layer only
//

std::string GetDefaultPrimBlockedReason(const UsdStageWeakPtr &stage) {
    if (stage->GetEditTarget().GetLayer() != stage->GetRootLayer()) {
        return "USD reads the default prim from the root layer only.\nChoose the root layer as edit target to change it.";
    }
    return {};
}

/// The session layer can author a default prim, USD ignores it
std::string GetIgnoredSessionDefaultPrimText(const UsdStageWeakPtr &stage) {
    const SdfLayerHandle session = stage->GetSessionLayer();
    if (!session || session->GetDefaultPrim().IsEmpty()) {
        return {};
    }
    return "The session layer sets it to " + session->GetDefaultPrim().GetString() + ", USD ignores it.";
}

/// The root prims of the stage, including the ones coming from the sublayers
TfToken DrawDefaultPrimCombo(const TfToken &current, const UsdStageWeakPtr &stage) {
    TfToken edited = current;
    if (ImGui::BeginCombo("##DefaultPrim", current.GetText())) {
        if (ImGui::Selectable("##NoDefaultPrim", current.IsEmpty())) {
            edited = TfToken();
        }
        for (const UsdPrim &rootPrim : stage->GetPseudoRoot().GetChildren()) {
            if (ImGui::Selectable(rootPrim.GetName().GetText(), rootPrim.GetName() == current)) {
                edited = rootPrim.GetName();
            }
        }
        ImGui::EndCombo();
    }
    return edited;
}

void DrawDefaultPrimRow(int &rowId, const UsdStageWeakPtr &stage) {
    const SdfLayerRefPtr rootLayer = stage->GetRootLayer();
    const TfToken current = rootLayer->GetDefaultPrim();
    const std::string blockedReason = GetDefaultPrimBlockedReason(stage);
    const StageMetadataSource source = current.IsEmpty() ? StageMetadataSource::Fallback : StageMetadataSource::RootLayer;
    ImGui::PushID(rowId++);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
    ImGui::TableSetColumnIndex(0);
    // Not disabled: with the session layer as edit target, "Edit strongest opinion" goes back to the root layer
    DrawMetadataMiniButton(stage, rootLayer, SdfFieldKeys->DefaultPrim, TfToken(), source);
    ImGui::BeginDisabled(!blockedReason.empty());
    ImGui::TableSetColumnIndex(1);
    DrawLabel("Default prim", !current.IsEmpty());
    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const TfToken edited = DrawDefaultPrimCombo(current, stage);
    if (edited != current) {
        if (edited.IsEmpty()) {
            ExecuteAfterDraw(&SdfLayer::ClearDefaultPrim, rootLayer);
        } else {
            ExecuteAfterDraw(&SdfLayer::SetDefaultPrim, rootLayer, edited);
        }
    }
    ImGui::EndDisabled();
    std::string tooltip = current.IsEmpty() ? "Not authored in the root layer" : "Set in the root layer";
    for (const std::string &line : {blockedReason, GetIgnoredSessionDefaultPrimText(stage)}) {
        if (!line.empty()) {
            tooltip += "\n" + line;
        }
    }
    DrawValueTooltip(tooltip);
    ImGui::PopID();
}

//
// Dictionaries: shown and edited in the edit target layer, writing the merged dictionary would copy the keys of the
// other layer into it
//

void DrawDictionaryRows(int &rowId, const UsdStageWeakPtr &stage, const TfToken &key) {
    const SdfLayerHandle editLayer = stage->GetEditTarget().GetLayer();
    DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, editLayer->GetPseudoRoot(), key);
    if (editLayer == stage->GetRootLayer() && LayerHasField(stage->GetSessionLayer(), key)) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImVec4(ColorAttributeShadowed), "The session layer also sets %s, its keys win over these",
                           key.GetText());
    }
}

//
// Value editors
//

VtValue DrawUpAxisCombo(const VtValue &value) {
    const TfToken current = value.IsHolding<TfToken>() ? value.UncheckedGet<TfToken>() : TfToken();
    VtValue edited;
    if (ImGui::BeginCombo("##UpAxis", current.GetText())) {
        for (const TfToken &axis : {UsdGeomTokens->y, UsdGeomTokens->z}) {
            if (ImGui::Selectable(axis.GetText(), axis == current) && axis != current) {
                edited = VtValue(axis);
            }
        }
        ImGui::EndCombo();
    }
    return edited;
}

VtValue DrawMultilineText(const VtValue &value) {
    std::string text = value.IsHolding<std::string>() ? value.UncheckedGet<std::string>() : std::string();
    ImGui::InputTextMultiline("##Text", &text, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 3));
    return ImGui::IsItemDeactivatedAfterEdit() ? VtValue(text) : VtValue();
}

VtValue DrawScalar(const VtValue &value) { return DrawVtValue("##Value", value); }

VtValue GetStageMetadata(const UsdStageWeakPtr &stage, const TfToken &key) {
    VtValue value;
    stage->GetMetadata(key, &value);
    return value;
}

const std::vector<StageMetadataField> &GetStageMetadataFields() {
    static const std::vector<StageMetadataField> fields = {
        {"Up axis", UsdGeomTokens->upAxis, TfToken(),
         [](const UsdStageWeakPtr &stage) { return VtValue(UsdGeomGetStageUpAxis(stage)); }, DrawUpAxisCombo},
        {"Meters per unit", UsdGeomTokens->metersPerUnit, TfToken(),
         [](const UsdStageWeakPtr &stage) { return VtValue(UsdGeomGetStageMetersPerUnit(stage)); }, DrawScalar},
        {"Start timecode", SdfFieldKeys->StartTimeCode, SdfFieldKeys->StartFrame,
         [](const UsdStageWeakPtr &stage) { return VtValue(stage->GetStartTimeCode()); }, DrawScalar},
        {"End timecode", SdfFieldKeys->EndTimeCode, SdfFieldKeys->EndFrame,
         [](const UsdStageWeakPtr &stage) { return VtValue(stage->GetEndTimeCode()); }, DrawScalar},
        {"Frames per second", SdfFieldKeys->FramesPerSecond, TfToken(),
         [](const UsdStageWeakPtr &stage) { return VtValue(stage->GetFramesPerSecond()); }, DrawScalar},
        {"Frame precision", SdfFieldKeys->FramePrecision, TfToken(),
         [](const UsdStageWeakPtr &stage) { return GetStageMetadata(stage, SdfFieldKeys->FramePrecision); }, DrawScalar},
        {"TimeCodes per second", SdfFieldKeys->TimeCodesPerSecond, TfToken(),
         [](const UsdStageWeakPtr &stage) { return VtValue(stage->GetTimeCodesPerSecond()); }, DrawScalar},
        {"Render settings prim path", UsdRenderTokens->renderSettingsPrimPath, TfToken(),
         [](const UsdStageWeakPtr &stage) { return GetStageMetadata(stage, UsdRenderTokens->renderSettingsPrimPath); },
         DrawScalar},
        {"Documentation", SdfFieldKeys->Documentation, TfToken(),
         [](const UsdStageWeakPtr &stage) { return GetStageMetadata(stage, SdfFieldKeys->Documentation); },
         DrawMultilineText},
        {"Comment", SdfFieldKeys->Comment, TfToken(),
         [](const UsdStageWeakPtr &stage) { return GetStageMetadata(stage, SdfFieldKeys->Comment); }, DrawMultilineText},
    };
    return fields;
}

} // namespace

void DrawStageMetadata(const UsdStageWeakPtr &stage) {
    if (!stage || !stage->GetEditTarget().GetLayer()) {
        return;
    }
    if (ImGui::CollapsingHeader("Core Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (BeginThreeColumnsTable("##DrawStageMetadata")) {
            SetupThreeColumnsTable(false);
            int rowId = 0;
            DrawDefaultPrimRow(rowId, stage);
            for (const StageMetadataField &field : GetStageMetadataFields()) {
                DrawFieldRow(rowId, stage, field);
            }
            DrawDictionaryRows(rowId, stage, SdfFieldKeys->CustomLayerData);
            DrawDictionaryRows(rowId, stage, SdfFieldKeys->AssetInfo);
            EndThreeColumnsTable();
        }
    }
}
