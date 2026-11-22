#include <iostream>
#include <unordered_map>
#include <array>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/pcp/primIndex.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/base/tf/stringUtils.h>
#include "Gui.h"
#include "UsdPrimEditor.h"
#include "VtValueEditor.h"
#include "Constants.h"
#include "Commands.h"
#include "UsdHelpers.h"
#include "ImGuiHelpers.h"
#include "TableLayouts.h"
#include "AssetPathBrowserDialog.h" // includes ModalDialogs.h
#include "ShaderDefinition.h"
#include "StageMetadataEditor.h"

PXR_NAMESPACE_USING_DIRECTIVE


/// A material browser which keeps a cache of the current material list until it is reset.
///
struct MaterialList {
    // Returns true if a material was selected. SdfPath() is the empty material.
    bool Draw(const UsdStageWeakPtr &stage, SdfPath &selected) {
        bool ret = false;
        if (!_cacheValid) {
            FindMaterials(stage);
        }
        if (_materialPaths.empty()) {
            ImGui::Text("no materials found in the stage");
        } else {
            if (ImGui::Selectable(ICON_FA_TRASH " unbind material", false)) {
                selected = SdfPath();
                ret = true;
            }
            for (auto &materialPath : _materialPaths) {
                ImGui::PushID(materialPath.GetString().c_str());
                if (ImGui::Selectable(materialPath.GetString().c_str(), false)) {
                    selected = materialPath;
                    ret = true;
                }
                ImGui::PopID();
            }
        }
        return ret;
    }

    void ResetCache() {
        if (_cacheValid) {
            _materialPaths.clear();
            _cacheValid = false;
        }
    }

  private:
    // Fills the materialPaths vector with the materials found in prim's stage
    void FindMaterials(const UsdStageWeakPtr &stage) {
        _cacheValid = true;
        if (!stage)
            return;

        UsdPrimRange range = stage->Traverse();
        for (const auto &prim : range) {
            if (prim.IsA<UsdShadeMaterial>()) {
                _materialPaths.push_back(prim.GetPath());
            }
        }
    }

    bool _cacheValid = false;
    std::vector<SdfPath> _materialPaths;
};



/// Very basic ui to create a connection
struct CreateConnectionDialog : public ModalDialog {

    CreateConnectionDialog(const UsdAttribute &attribute) : _attribute(attribute){};
    ~CreateConnectionDialog() override {}

    void Draw() override {
        ImGui::Text("Create connection for %s", _attribute.GetPath().GetString().c_str());
        ImGui::InputText("Path", &_connectionEndPoint);
        DrawModalButtonsOkCancel([&]() {
            ExecuteAfterDraw(&UsdAttribute::AddConnection, _attribute, SdfPath(_connectionEndPoint),
                             UsdListPositionBackOfPrependList);
        });
    }
    const char *DialogId() const override { return "Attribute connection"; }

    UsdAttribute _attribute;
    std::string _connectionEndPoint;
};

struct CreateRelationshipDialog : public ModalDialog {
    
    CreateRelationshipDialog(const UsdRelationship &relationship)
    : _relationship(relationship), _listPositionToString({{UsdListPositionFrontOfAppendList, "Front of append list"},
                                                          {UsdListPositionBackOfAppendList, "Back of append list"},
                                                          {UsdListPositionFrontOfPrependList, "Front of prepend list"},
                                                          {UsdListPositionBackOfPrependList, "Back of prepend list"}}){};
    ~CreateRelationshipDialog() override {}

    void Draw() override {
        ImGui::Text("%s", _relationship.GetPath().GetString().c_str());
        if (ImGui::BeginCombo("Position in list", _listPositionToString[_listPosition].c_str())) {
            for (const auto &pos : _listPositionToString) {
                if (ImGui::Selectable(pos.second.c_str())) {
                    _listPosition = pos.first;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::InputText("Relationship path", &_relationshipPath);
        const bool isValid =
            SdfPath::IsValidPathString(_relationshipPath) && SdfPath(_relationshipPath) != SdfPath::AbsoluteRootPath();
        if (isValid) {
            ImGui::TextColored(ImVec4(0.0, 0.8, 0.0, 1.0), "Valid path");
        } else {
            ImGui::TextColored(ImVec4(0.8, 0.0, 0.0, 1.0), "Invalid path");
        }
        DrawModalButtonsOkCancel([&]() {
            if (isValid) {
                ExecuteAfterDraw(&UsdRelationship::AddTarget, _relationship, SdfPath(_relationshipPath), _listPosition);
            }
        });
    }
    const char *DialogId() const override { return "Create Relationship"; }

    UsdRelationship _relationship;
    std::string _relationshipPath;
    UsdListPosition _listPosition = UsdListPositionFrontOfAppendList;
    std::map<UsdListPosition, std::string> _listPositionToString;

};

/// Disabled items don't report hovering, but imgui still records the disabled item under the mouse.
/// Shows the reason when that item is in the current window, its children or the popups opened from them.
static void DrawEditBlockedTooltip(const std::string &reason) {
    const ImGuiContext &g = *GImGui;
    if (reason.empty() || g.HoveredId == 0 || !g.HoveredIdIsDisabled || !g.HoveredWindow || !IsMouseRestingForHint())
        return;
    if (ImGui::IsWindowChildOf(g.HoveredWindow, ImGui::GetCurrentWindow(), true, false)) {
        ImGui::SetTooltip("%s", reason.c_str());
    }
}

/// One line under the header explaining why everything is greyed
static void DrawEditBlockedBanner(const std::string &reason) {
    if (reason.empty())
        return;
    const std::string firstLine = reason.substr(0, reason.find('\n'));
    ImGui::TextColored(ImVec4(ColorHighlight), ICON_FA_LOCK " %s", firstLine.c_str());
    if (IsItemHoveredForHint()) {
        ImGui::SetTooltip("%s", reason.c_str());
    }
}

/// Why an edit of the attribute value won't show, with the way to fix it from the property editor, empty when it will show
static std::string GetShadowedValueReason(const UsdAttribute &attribute, UsdTimeCode time,
                                          const OpinionStrength &editTargetStrength) {
    const std::optional<ShadowingOpinion> opinion = FindShadowingOpinion(attribute, time, editTargetStrength);
    if (!opinion)
        return {};
    return FormatShadowingOpinion(*opinion, attribute.GetStage()) + "\nUse (a) > Edit strongest opinion to edit it there.";
}

/// The shadowed value tooltip shows when hovering the value column of the current row
static void DrawShadowedValueTooltip(const std::string &reason) {
    if (!reason.empty() && IsMouseRestingForHint() && ImGui::TableGetHoveredColumn() == 2 && ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex()) {
        ImGui::SetTooltip("%s", reason.c_str());
    }
}

/// Select and draw the appropriate editor depending on the type, metada and so on.
/// Returns the modified value or VtValue
static VtValue DrawAttributeValue(const std::string &label, UsdAttribute &attribute, const VtValue &value) {
    // If the attribute is a TfToken, it might have an "allowedTokens" metadata
    // We assume that the attribute is a token if it has allowedToken, but that might not hold true
    VtValue allowedTokens;
    attribute.GetMetadata(TfToken("allowedTokens"), &allowedTokens);
    if (!allowedTokens.IsEmpty()) {
        return DrawTfToken(label, value, allowedTokens);
    }
    //
    if (attribute.GetRoleName() == TfToken("Color")) {
        // TODO: color values can be "dragged" they should be stored between
        // BeginEdition/EndEdition
        // It needs some refactoring to know when the widgets starts and stop edition
        return DrawColorValue(label, value);
    }
    // Asset path values get a mini file browser button anchored to the current edit target.
    if (value.IsHolding<SdfAssetPath>()) {
        if (ImGui::Button(ICON_FA_FILE)) {
            DrawModalDialog<AssetPathBrowserDialog>(attribute);
        }
        ImGui::SameLine();
    }
    return DrawVtValue(label, value);
}

template <typename PropertyT> static std::string GetDisplayName(const PropertyT &property) {
    return property.GetNamespace().GetString() + (property.GetNamespace() == TfToken() ? std::string() : std::string(":")) +
           property.GetBaseName().GetString();
}

void DrawAttributeTypeInfo(const UsdAttribute &attribute) {
    auto attributeTypeName = attribute.GetTypeName();
    auto attributeRoleName = attribute.GetRoleName();
    ImGui::Text("%s(%s)", attributeRoleName.GetString().c_str(), attributeTypeName.GetAsToken().GetString().c_str());
}

// Right align is not used at the moment, but keeping the code as this is useful for quick layout testing
inline void RightAlignNextItem(const char *str) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(str).x - ImGui::GetScrollX() -
                         2 * ImGui::GetStyle().ItemSpacing.x);
}

void DrawAttributeDisplayName(const UsdAttribute &attribute) {
    const std::string displayName = GetDisplayName(attribute);
    ImGui::Text("%s", displayName.c_str());
}

// Connected attribute path, clicking on it selects its prim
static void DrawConnectionLink(const UsdStageWeakPtr &stage, const SdfPath &path) {
    ScopedStyleColor linkColor(ImGuiCol_Text, ImVec4(ColorAttributeConnection));
    if (NavigationLink((ICON_FA_LINK " " + path.GetString()).c_str())) {
        ExecuteAfterDraw<EditorSetSelection>(stage, path.GetPrimPath());
    }
}

// Connections of the attribute, with a button to remove them
static bool DrawAttributeConnections(const UsdAttribute &attribute, bool editable) {
    SdfPathVector sources;
    attribute.GetConnections(&sources);
    for (auto &connection : sources) {
        ImGui::PushID(connection.GetString().c_str());
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button(ICON_FA_TRASH)) {
            ExecuteAfterDraw(&UsdAttribute::RemoveConnection, attribute, connection);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        DrawConnectionLink(attribute.GetStage(), connection);
        ImGui::PopID();
    }
    return !sources.empty();
}

void DrawAttributeValueAtTime(UsdAttribute &attribute, UsdTimeCode currentTime, bool editable) {
    const std::string attributeLabel = GetDisplayName(attribute);
    VtValue value;
    // TODO: On the lower spec mac, this call appears to be really slow with some attributes
    //       AttributeQuery could be a solution but doesn't update with external data updates
    const bool HasValue = attribute.Get(&value, currentTime);

    if (HasValue) {
        ImGui::BeginDisabled(!editable);
        VtValue modified = DrawAttributeValue(attributeLabel, attribute, value);
        ImGui::EndDisabled();
        if (!modified.IsEmpty()) {
            ExecuteAfterDraw<AttributeSet>(attribute, modified,
                                           attribute.GetNumTimeSamples() ? currentTime : UsdTimeCode::Default());
        }
    }

    const bool HasConnections = attribute.HasAuthoredConnections() && DrawAttributeConnections(attribute, editable);

    if (!HasValue && !HasConnections) {
        ImGui::TextColored(ImVec4({0.5, 0.5, 0.5, 0.5}), "no value");
    }
}

void DrawUsdRelationshipDisplayName(const UsdRelationship &relationship) {
    std::string relationshipName = GetDisplayName(relationship);
    ImVec4 attributeNameColor = relationship.IsAuthored() ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored);
    ImGui::TextColored(ImVec4(attributeNameColor), "%s", relationshipName.c_str());
}

void DrawUsdRelationshipList(const UsdRelationship &relationship, bool editable) {
    SdfPathVector targets;
    // relationship.GetForwardedTargets(&targets);
    // for (const auto &path : targets) {
    //    ImGui::TextColored(ImVec4(AttributeRelationshipColor), "%s", path.GetString().c_str());
    //}
    relationship.GetTargets(&targets);
    if (!targets.empty()) {
        ImGui::PushID(relationship.GetPath().GetString().c_str());
        if (ImGui::BeginListBox("##Relationship", ImVec2(-FLT_MIN, targets.size() * 25))) {
            for (const auto &path : targets) {
                ImGui::PushID(path.GetString().c_str());
                ImGui::BeginDisabled(!editable);
                if (ImGui::Button(ICON_FA_TRASH)) {
                    ExecuteAfterDraw(&UsdRelationship::RemoveTarget, relationship, path);
                }
                ImGui::SameLine();
                std::string buffer = path.GetString();
                ImGui::InputText("##EditRelation", &buffer);
                if (ImGui::IsItemDeactivatedAfterEdit() && buffer != path.GetString()) {
                    // RemoveTarget and add newTarget
                    ExecuteAfterDraw<RelationshipReplace>(relationship, path, SdfPath(buffer));
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::PopID();
    }
}

void DrawPropertyStack(const UsdProperty &property, UsdTimeCode currentTime) {
    SdfPropertySpecHandleVector properties = property.GetPropertyStack(currentTime);
    int itemId = 0;
    for (const auto &prop : properties) {
        const SdfPath& propPath = prop.GetSpec().GetPath();
        std::string site = prop.GetSpec().GetLayer()->GetDisplayName() + " " + propPath.GetString();
        ImGui::PushID(itemId++);
        if (ImGui::MenuItem(site.c_str())) {
            ExecuteAfterDraw<EditorSetSelection>(prop.GetSpec().GetLayer(), propPath);
        }
        ImGui::PopID();
    }
}

inline
void SetEditTargetOnPropertyStrongestOpinion(const UsdProperty& property, UsdTimeCode currentTime) {
    auto pcpIndex = property.GetPrim().GetPrimIndex();
    if (pcpIndex.IsValid()) {
        SdfPropertySpecHandleVector properties = property.GetPropertyStack(currentTime);
        if (!properties.empty()) {
            auto prop = properties[0];
            if (prop) {
                PcpNodeRef node = pcpIndex.GetNodeProvidingSpec(prop->GetLayer(), prop->GetPath().GetPrimPath());
                if (node) {
                    UsdEditTarget editTarget(prop->GetLayer(), node);
                    ExecuteAfterDraw<EditorSetEditTarget>(property.GetStage(), editTarget);
                }
            }
        }
    }
}

inline void SetEditTargetOnVariantSetStrongestOpinion(const UsdPrim& prim, const std::string& variantSetName) {
    auto pcpIndex = prim.GetPrimIndex();
    if (!pcpIndex.IsValid()) return;
    for (const auto& spec : prim.GetPrimStack()) {
        if (!spec) continue;
        if (spec->GetVariantSelections().count(variantSetName)) {
            PcpNodeRef node = pcpIndex.GetNodeProvidingSpec(spec->GetLayer(), spec->GetPath().GetPrimPath());
            if (node) {
                ExecuteAfterDraw<EditorSetEditTarget>(prim.GetStage(), UsdEditTarget(spec->GetLayer(), node));
                return;
            }
        }
    }
}

/// Specialization for DrawPropertyMiniButton, between UsdAttribute and UsdRelashionship
template <typename UsdPropertyT> const char *SmallButtonLabel();
template <> const char *SmallButtonLabel<UsdAttribute>() { return "(a)"; };
template <> const char *SmallButtonLabel<UsdRelationship>() { return "(r)"; };

template <typename UsdPropertyT> void DrawMenuClearAuthoredValues(UsdPropertyT &property){};
template <> void DrawMenuClearAuthoredValues(UsdAttribute &attribute) {
    if (attribute.IsAuthored()) {
        if (ImGui::MenuItem(ICON_FA_ERASER " Clear values")) {
            ExecuteAfterDraw(&UsdAttribute::Clear, attribute);
        }
    }
}

template <> void DrawMenuClearAuthoredValues(UsdRelationship &relationship) {
    if (relationship.HasAuthoredTargets()) {
        if (ImGui::MenuItem(ICON_FA_ERASER " Clear targets")) {
            ExecuteAfterDraw(&UsdRelationship::ClearTargets, relationship, true);
        }
    }
}

template <typename UsdPropertyT> void DrawMenuBlockValues(UsdPropertyT &property){};
template <> void DrawMenuBlockValues(UsdAttribute &attribute) {
    if (ImGui::MenuItem(ICON_FA_BAN " Block values")) {
        ExecuteAfterDraw(&UsdAttribute::Block, attribute);
    }
}

template <typename UsdPropertyT> void DrawMenuRemoveProperty(UsdPropertyT &property){};
template <> void DrawMenuRemoveProperty(UsdAttribute &attribute) {
    if (ImGui::MenuItem(ICON_FA_TRASH " Remove edit")) {
        ExecuteAfterDraw(&UsdPrim::RemoveProperty, attribute.GetPrim(), attribute.GetName());
    }
}

template <typename UsdPropertyT> void DrawMenuSetKey(UsdPropertyT &property, UsdTimeCode currentTime){};
template <> void DrawMenuSetKey(UsdAttribute &attribute, UsdTimeCode currentTime) {
    if (attribute.GetVariability() == SdfVariabilityVarying && attribute.HasValue() && ImGui::MenuItem(ICON_FA_KEY " Set key")) {
        VtValue value;
        attribute.Get(&value, currentTime);
        ExecuteAfterDraw<AttributeSet>(attribute, value, currentTime);
    }
}

// TODO Share the code,
static bool DrawPropertyMiniButton(const char *btnStr, const ImVec4 &btnColor = ImVec4(ColorMiniButtonUnauthored)) {
    ScopedStyleColor miniButtonStyle(ImGuiCol_Text, btnColor, ImGuiCol_Button, ImVec4(ColorTransparent) );
    return ImGui::SmallButton(btnStr);
}

template <typename UsdPropertyT> void DrawMenuEditConnection(UsdPropertyT &property) {}
template <> void DrawMenuEditConnection(UsdAttribute &attribute) {
    if (ImGui::MenuItem(ICON_FA_LINK " Create connection")) {
        DrawModalDialog<CreateConnectionDialog>(attribute);
    }
}


template <typename UsdPropertyT> void DrawMenuCreateValue(UsdPropertyT &property){};

template <> void DrawMenuCreateValue(UsdAttribute &attribute) {
    if (!attribute.HasValue()) {
        if (ImGui::MenuItem(ICON_FA_PLUS_CIRCLE " Create value")) {
            ExecuteAfterDraw<AttributeCreateDefaultValue>(attribute);
        }
    }
}

template <> void DrawMenuCreateValue(UsdRelationship &relationship) {
    if (ImGui::MenuItem(ICON_FA_PLUS_CIRCLE " Create relationship")) {
        DrawModalDialog<CreateRelationshipDialog>(relationship);
    }
}

// Property mini button, should work with UsdProperty, UsdAttribute and UsdRelationShip
template <typename UsdPropertyT>
void DrawPropertyMiniButton(UsdPropertyT &property, const UsdEditTarget &editTarget, UsdTimeCode currentTime,
                            bool editable) {
    ImVec4 propertyColor =
        property.IsAuthoredAt(editTarget) ? ImVec4(ColorMiniButtonAuthored) : ImVec4(ColorMiniButtonUnauthored);
    DrawPropertyMiniButton(SmallButtonLabel<UsdPropertyT>(), propertyColor);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        // The authoring entries are greyed, the copy and navigation ones stay available
        ImGui::BeginDisabled(!editable);
        DrawMenuSetKey(property, currentTime);
        DrawMenuCreateValue(property);
        DrawMenuClearAuthoredValues(property);
        DrawMenuBlockValues(property);
        DrawMenuRemoveProperty(property);
        DrawMenuEditConnection(property);
        ImGui::EndDisabled();
        if (ImGui::MenuItem(ICON_FA_COPY " Copy attribute path")) {
            ImGui::SetClipboardText(property.GetPath().GetString().c_str());
        }
        if (ImGui::BeginMenu(ICON_FA_MOUSE_POINTER " Select SdfAttribute")) {
            DrawPropertyStack(property, currentTime);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Edit strongest opinion")) {
            SetEditTargetOnPropertyStrongestOpinion(property, currentTime);
        }
        ImGui::EndPopup();
    }
}

bool DrawVariantSetsCombos(UsdPrim &prim) {
    int buttonID = 0;
    if (!prim.HasVariantSets())
        return false;

    const UsdEditTarget editTarget = prim.GetStage()->GetEditTarget();
    if (!editTarget.GetLayer()) {
        ImGui::Text("No edit target");
        return false;
    }

    auto variantSets = prim.GetVariantSets();

    const SdfPath targetPath = editTarget.MapToSpecPath(prim.GetPath());
    const bool editingInVariant = targetPath.ContainsPrimVariantSelection();
    const bool editable = IsPrimEditable(prim);

    if (editingInVariant) {
        ImGui::BeginDisabled(true);
    }

    if (ImGui::BeginTable("##DrawVariantSetsCombos", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        // make it relative to font size
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("VariantSet");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableHeadersRow();

        auto editTargetPrimSpec = editTarget.GetLayer()->GetPrimAtPath(targetPath);

        for (auto variantSetName : variantSets.GetNames()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // Variant set mini button --- TODO move code from this function
            auto variantSet = variantSets.GetVariantSet(variantSetName);
            bool isAuthoredAtEditTarget = editTargetPrimSpec &&
                editTargetPrimSpec->GetVariantSelections().count(variantSetName) > 0;
            ImVec4 variantColor =
                isAuthoredAtEditTarget ? ImVec4(ColorMiniButtonAuthored) : ImVec4(ColorMiniButtonUnauthored);
            ImGui::PushID(buttonID++);
            DrawPropertyMiniButton("(v)", variantColor);
            ImGui::PopID();
            if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
                if (ImGui::MenuItem("Edit strongest opinion")) {
                    SetEditTargetOnVariantSetStrongestOpinion(prim, variantSetName);
                }
                if (ImGui::MenuItem("Clear variant selection", nullptr, false, editable)) {
                    ExecuteAfterDraw(&UsdVariantSet::ClearVariantSelection, variantSet);
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", variantSetName.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(!editable);
            if (ImGui::BeginCombo(variantSetName.c_str(), variantSet.GetVariantSelection().c_str())) {
                for (auto variant : variantSet.GetVariantNames()) {
                    if (ImGui::Selectable(variant.c_str(), false)) {
                        ExecuteAfterDraw(&UsdVariantSet::SetVariantSelection, variantSet, variant);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopItemWidth();
        }
        ImGui::EndTable();
    }
    if (editingInVariant) {
        ImGui::EndDisabled();
        if (IsItemHoveredForHint(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Variant selection is fixed by the current edit target");
    }
    return true;
}

/// Clearing only removes the opinion in the edit target, so that's what we check, not the composed value.
static bool IsAuthoredInEditTarget(const UsdPrim &prim, const TfToken &field) {
    const SdfPrimSpecHandle primSpec = prim.GetStage()->GetEditTarget().GetPrimSpecForScenePath(prim.GetPath());
    return primSpec && primSpec->HasField(field);
}

/// Draws the clear button and the label, and leaves the cursor in the value column
static void BeginPrimMetadataRow(const UsdPrim &prim, const TfToken &field, const char *label) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
    ImGui::PushID(label);
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginDisabled(!IsAuthoredInEditTarget(prim, field));
    if (DrawPropertyMiniButton(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&UsdPrim::ClearMetadata, prim, field);
    }
    ImGui::EndDisabled();
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", label);
    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static void EndPrimMetadataRow() { ImGui::PopID(); }

static void DrawPrimKindRow(const UsdPrim &prim) {
    BeginPrimMetadataRow(prim, SdfFieldKeys->Kind, "Kind");
    TfToken primKind;
    prim.GetKind(&primKind);
    if (ImGui::BeginCombo("##Kind", primKind.GetText())) {
        for (const TfToken &kind : KindRegistry::GetAllKinds()) {
            if (ImGui::Selectable(kind.GetText(), kind == primKind)) {
                ExecuteAfterDraw(&UsdPrim::SetKind, prim, kind);
            }
        }
        ImGui::EndCombo();
    }
    EndPrimMetadataRow();
}

static void DrawPrimActiveRow(const UsdPrim &prim) {
    BeginPrimMetadataRow(prim, SdfFieldKeys->Active, "Active");
    bool isActive = prim.IsActive();
    if (ImGui::Checkbox("##Active", &isActive)) {
        ExecuteAfterDraw(&UsdPrim::SetActive, prim, isActive);
    }
    EndPrimMetadataRow();
}

static void DrawPrimInstanceableRow(const UsdPrim &prim) {
    BeginPrimMetadataRow(prim, SdfFieldKeys->Instanceable, "Instanceable");
    bool isInstanceable = prim.IsInstanceable();
    if (ImGui::Checkbox("##Instanceable", &isInstanceable)) {
        ExecuteAfterDraw(&UsdPrim::SetInstanceable, prim, isInstanceable);
    }
    EndPrimMetadataRow();
}

bool DrawPrimMetadata(UsdPrim &prim) {
    if (prim.IsPseudoRoot())
        return false;

    if (ImGui::BeginTable("##DrawPrimMetadata", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("Prim metadata");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::BeginDisabled(!IsPrimEditable(prim));
        DrawPrimKindRow(prim);
        DrawPrimActiveRow(prim);
        DrawPrimInstanceableRow(prim);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    return true;
}

/// An empty type can't be authored over a type coming from a weaker layer, we can only remove the local opinion.
static void ApplyPrimType(const UsdPrim &prim, const std::string &typeName) {
    if (!typeName.empty()) {
        ExecuteAfterDraw(&UsdPrim::SetTypeName, prim, TfToken(typeName));
    } else if (IsAuthoredInEditTarget(prim, SdfFieldKeys->TypeName)) {
        ExecuteAfterDraw(&UsdPrim::ClearMetadata, prim, SdfFieldKeys->TypeName);
    }
}

/// The type name drawn as a label sized to its text, clicking on it opens the list of types.
static void DrawPrimTypeCombo(const UsdPrim &prim) {
    const std::string &currentType = prim.GetTypeName().GetString();
    const char *preview = currentType.empty() ? "(no type)" : currentType.c_str();
    const auto &allTypes = GetAllSpecTypeNames();
    static int selected = 0;

    ImGui::BeginDisabled(!IsPrimEditable(prim));
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(preview).x + ImGui::GetStyle().FramePadding.x * 2.f);
    // The popup is as wide as the combo by default, which is too narrow to read the types
    const float popupSize = ImGui::GetFontSize() * 20.f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupSize, 0.f), ImVec2(FLT_MAX, popupSize));
    // Looks like a label until hovered. The popup content is drawn with the same style, so it keeps its background when open
    ImGui::PushID("##PrimType");
    const bool isPopupOpen = ImGui::IsPopupOpen("##ComboPopup");
    ImGui::PopID();
    const ImVec4 frameColor = isPopupOpen ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg) : ImVec4(ColorTransparent);
    ScopedStyleColor transparentFrame(ImGuiCol_FrameBg, frameColor);
    if (ComboWithFilter("##PrimType", preview, allTypes, &selected, ImGuiComboFlags_NoArrowButton)) {
        if (allTypes[selected] != currentType) {
            ApplyPrimType(prim, allTypes[selected]);
        }
    }
    ImGui::EndDisabled();
}

bool DrawAssetInfo(UsdPrim &prim) {
    auto assetInfo = prim.GetAssetInfo();
    if (assetInfo.empty())
        return false;

    const bool editable = IsPrimEditable(prim);
    if (ImGui::BeginTable("##DrawAssetInfo", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        //TODO  make it relative
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("Asset info");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableHeadersRow();
        int rowId = 0;
        TF_FOR_ALL(keyValue, assetInfo) {
            ImGui::TableNextRow();
            ImGui::PushID(rowId++);
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", keyValue->first.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(!editable);
            VtValue modified = DrawVtValue(keyValue->first, keyValue->second);
            ImGui::EndDisabled();
            if (!modified.IsEmpty()) {
                ExecuteAfterDraw(&UsdPrim::SetAssetInfoByKey, prim, TfToken(keyValue->first), modified);
            }
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return true;
}

/// The sections of the property editor, in drawing order, the Show menu hides them
enum class PrimEditorSection {
    Instancing,
    ShaderInputs,
    Metadata,
    AssetInfo,
    MaterialBindings,
    Variants,
    Transform,
    Attributes,
    Relationships,
    Count
};

struct PrimEditorSectionToggle {
    const char *label;
    bool shown;
};

/// Not saved in the settings on purpose: we'll see with use if it should be global or per prim type
static std::array<PrimEditorSectionToggle, static_cast<size_t>(PrimEditorSection::Count)> &GetPrimEditorSections() {
    static std::array<PrimEditorSectionToggle, static_cast<size_t>(PrimEditorSection::Count)> sections = {{
        {"Instancing", true},
        {"Shader inputs", true},
        {"Metadata", true},
        {"Asset info", true},
        {"Material bindings", true},
        {"Variants", true},
        {"Transform", true},
        {"Attributes", true},
        {"Relationships", true},
    }};
    return sections;
}

static bool IsSectionShown(PrimEditorSection section) { return GetPrimEditorSections()[static_cast<size_t>(section)].shown; }

/// Draws the section when it's shown, followed by a separator when it drew something
template <typename DrawFuncT> static void DrawSection(PrimEditorSection section, DrawFuncT &&draw) {
    if (IsSectionShown(section) && draw()) {
        ImGui::Separator();
    }
}

static void DrawPropertyEditorMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Show")) {
            for (PrimEditorSectionToggle &section : GetPrimEditorSections()) {
                ImGui::MenuItem(section.label, nullptr, &section.shown);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

static bool DrawPrimSchemas(UsdPrim &prim, bool &edit) {
    // Create the text from the schemas
    static std::string schemaString; // static to avoid reallocating
    schemaString.clear();
    for (const auto &schema:prim.GetAppliedSchemas()) {
        schemaString += schema.GetString() + " ";
    }
    if (edit) {
        ImGui::InputText("##AppliedSchemas", &schemaString);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            std::vector<std::string> editedSchemas = TfStringTokenize(schemaString);
            ExecuteAfterDraw<PrimApplySchemas>(prim, editedSchemas);
            edit = false;
        }
    } else {
        ImGui::Text("%s", schemaString.c_str());
    }
    return true;
}

/// Draws a menu list of all the sublayers, indented to reveal the parenting
static void DrawEditTargetSubLayersMenuItems(UsdStageWeakPtr stage, SdfLayerHandle layer, int indent = 0) {
    if (layer) {
        std::vector<std::string> subLayers = layer->GetSubLayerPaths();
        for (int layerId = 0; layerId < subLayers.size(); ++layerId) {
            const std::string &subLayerPath = subLayers[layerId];
            auto subLayer = SdfLayer::FindOrOpenRelativeToLayer(layer, subLayerPath);
            if (!subLayer) {
                subLayer = SdfLayer::FindOrOpen(subLayerPath);
            }
            const std::string layerName = std::string(indent, ' ') + (subLayer ? subLayer->GetDisplayName() : subLayerPath);
            ImGui::PushID(layerId);
            if (subLayer) {
                if (ImGui::MenuItem(layerName.c_str())) {
                    ExecuteAfterDraw<EditorSetEditTarget>(stage, UsdEditTarget(subLayer));
                }
            } else {
                ImGui::TextColored(ImVec4(1.0, 0.0, 0.0, 1.0), "%s", layerName.c_str());
            }
            DrawEditTargetSubLayersMenuItems(stage, subLayer, indent + 4);
            ImGui::PopID();
        }
    }
}

bool DrawMaterialBindings(const UsdPrim &prim) {
#if (PXR_VERSION < 2208)
    return false;
#else
    if (!prim || prim.IsA<UsdShadeMaterial>() || prim.IsA<UsdShadeShader>())
        return false;

    UsdShadeMaterialBindingAPI materialBindingAPI(prim);
    const bool editable = IsPrimEditable(prim);
    if (ImGui::BeginTable("##DrawPropertyEditorHeader", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("Material Bindings");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        UsdShadeMaterial material;
        for (const auto &purpose : materialBindingAPI.GetMaterialPurposes()) {
            const std::string &purposeName = purpose.GetString();
            ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);

            UsdShadeMaterialBindingAPI::DirectBinding directBinding = materialBindingAPI.GetDirectBinding(purpose);
            bool hasDirectBinding = directBinding.GetMaterial().GetPrim().IsValid();

            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(purposeName.c_str());
            {
                UsdRelationship bindingRel = directBinding.GetBindingRel();
                bool isAuthoredAtEditTarget = bindingRel && bindingRel.IsAuthoredAt(prim.GetStage()->GetEditTarget());
                ImVec4 cogColor = isAuthoredAtEditTarget ? ImVec4(ColorMiniButtonAuthored) : ImVec4(ColorAttributeAuthored);
                ScopedStyleColor cogStyle(ImGuiCol_Text, cogColor, ImGuiCol_Button, ImVec4(ColorTransparent));
                ImGui::BeginDisabled(!editable);
                if (ImGui::Button(ICON_FA_PALETTE)) {
                    ImGui::OpenPopup("MaterialList");
                }
                ImGui::EndDisabled();
            }
            // TODO: we would like to copy/paste material path
            static MaterialList materialList; // We expect only one thread running this code
            if (ImGui::BeginPopup("MaterialList")) {
                SdfPath selectedMaterial;
                if (materialList.Draw(prim.GetStage(), selectedMaterial)) {
                    ExecuteAfterDraw<UsdAPIMaterialBind>(prim, selectedMaterial, purpose);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            } else {
                materialList.ResetCache();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", purposeName == "" ? "All purposes" : purposeName.c_str());

            ImGui::TableSetColumnIndex(2);
            material = materialBindingAPI.ComputeBoundMaterial(purpose);
            if (material) {
                // TODO: we would also like to copy/paste material path
                ImVec4 textColor = hasDirectBinding ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored);
                ScopedStyleColor linkColor(ImGuiCol_Text, textColor);
                if (NavigationLink(material.GetPrim().GetPath().GetText())) {
                    ExecuteAfterDraw<EditorSetSelection>(material.GetPrim().GetStage(), material.GetPrim().GetPath());
                }
            } else {
                ImGui::Text("unbound");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();

        return true;
    }
    return false;
#endif
}



// Second version of an edit target selector
void DrawUsdPrimEditTarget(const UsdPrim &prim) {
    if (!prim)
        return;
    ScopedStyleColor defaultStyle(DefaultColorStyle);

    // Collect variant options by traversing the PCP node graph.
    // For each node, map its path to the root namespace via GetMapToRoot().
    // A path that ContainsPrimVariantSelection() means the node lives inside
    // a variant context; we collect all variant-selecting prefixes of that path
    // so nested variants (variants inside variants) are each offered as a
    // separate menu entry with the full stacked variant spec path.
    struct VariantOption {
        SdfPath     varSpecPath; // e.g. /Root{lod=high} or /Root{lod=high}{mat=metal}
        std::string label;
    };
    std::vector<VariantOption> variantOptions;

    std::function<void(PcpNodeRef)> collectVariants = [&](PcpNodeRef node) {
        const auto mapToRoot = node.GetMapToRoot();
        const auto finc = mapToRoot.Evaluate();
        const SdfPath rootPath = finc.MapSourceToTarget(node.GetPath());
        if (rootPath.ContainsPrimVariantSelection()) {
            for (const SdfPath &prefix : rootPath.GetPrefixes()) {
                if (!prefix.IsPrimVariantSelectionPath())
                    continue;
                // deduplicate across nodes
                bool seen = false;
                for (const auto &opt : variantOptions)
                    if (opt.varSpecPath == prefix) { seen = true; break; }
                if (!seen)
                    variantOptions.push_back({prefix, prefix.GetString()});
            }
        }
        TF_FOR_ALL(childNode, node.GetChildrenRange()) { collectVariants(*childNode); }
    };
    collectVariants(prim.GetPrimIndex().GetRootNode());

    // When variants are available, "Session layer" and "Root layer" become
    // submenus with a direct entry plus one entry per variant context.
    // When there are no variants they remain flat menu items.
    auto drawLayerItems = [&](const SdfLayerHandle &layer) {
        if (ImGui::MenuItem("No variant")) {
            ExecuteAfterDraw<EditorSetEditTarget>(prim.GetStage(), UsdEditTarget(layer));
        }
        int id = 0;
        for (const auto &opt : variantOptions) {
            ImGui::PushID(id++);
            if (ImGui::MenuItem(opt.label.c_str())) {
                ExecuteAfterDraw<EditorSetEditTarget>(prim.GetStage(),
                                                      UsdEditTarget::ForLocalDirectVariant(layer, opt.varSpecPath));
            }
            ImGui::PopID();
        }
    };

    if (variantOptions.empty()) {
        if (ImGui::MenuItem("Session layer")) {
            ExecuteAfterDraw<EditorSetEditTarget>(prim.GetStage(), UsdEditTarget(prim.GetStage()->GetSessionLayer()));
        }
        if (ImGui::MenuItem("Root layer")) {
            ExecuteAfterDraw<EditorSetEditTarget>(prim.GetStage(), UsdEditTarget(prim.GetStage()->GetRootLayer()));
        }
    } else {
        if (ImGui::BeginMenu("Session layer")) {
            drawLayerItems(prim.GetStage()->GetSessionLayer());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Root layer")) {
            drawLayerItems(prim.GetStage()->GetRootLayer());
            ImGui::EndMenu();
        }
    }

    if (ImGui::BeginMenu("Sublayers")) {
        const auto &layer = prim.GetStage()->GetRootLayer();
        DrawEditTargetSubLayersMenuItems(prim.GetStage(), layer);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Selected prim arcs")) {
        using Query = pxr::UsdPrimCompositionQuery;
        auto filter = UsdPrimCompositionQuery::Filter();
        pxr::UsdPrimCompositionQuery arc(prim, filter);
        auto compositionArcs = arc.GetCompositionArcs();
        int widgetId = 0;
        for (auto a : compositionArcs) {
            // NOTE: we can use GetIntroducingLayer() and GetIntroducingPrimPath() to add more information
            if (a.GetTargetNode()) {
                std::string arcName = a.GetTargetNode().GetLayerStack()->GetIdentifier().rootLayer->GetDisplayName() + " " +
                                      a.GetTargetNode().GetPath().GetString();
                ImGui::PushID(widgetId++);
                if (ImGui::MenuItem(arcName.c_str())) {
                    ExecuteAfterDraw<EditorSetEditTarget>(
                        prim.GetStage(),
                        UsdEditTarget(a.GetTargetNode().GetLayerStack()->GetIdentifier().rootLayer, a.GetTargetNode()),
                        a.GetTargetNode().GetPath());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndMenu();
    }
}

// Testing
// TODO: this is a copied function that needs to be moved in the next release
static ImVec4 GetPrimColor(const UsdPrim &prim) {
    if (!prim.IsActive() || !prim.IsLoaded()) {
        return ImVec4(ColorPrimInactive);
    }
    if (prim.IsInstance()) {
        return ImVec4(ColorPrimInstance);
    }
    const auto hasCompositionArcs = prim.HasAuthoredReferences() || prim.HasAuthoredPayloads() || prim.HasAuthoredInherits() ||
                                    prim.HasAuthoredSpecializes() || prim.HasVariantSets();
    if (hasCompositionArcs) {
        return ImVec4(ColorPrimHasComposition);
    }
    if (prim.IsPrototype() || prim.IsInPrototype() || prim.IsInstanceProxy()) {
        return ImVec4(ColorPrimPrototype);
    }
    return ImVec4(ColorPrimDefault);
}

// Forward declaration
void OpenContainerSheet(const UsdPrim &prim);

void DrawUsdPrimHeader(UsdPrim &prim) {
    auto editTarget = prim.GetStage()->GetEditTarget();
    const SdfPath targetPath = editTarget.MapToSpecPath(prim.GetPath());
    static bool editSchemas = false;
    if (ImGui::BeginTable("##DrawPropertyEditorHeader", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        
        //ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        // ImGui::TableSetColumnIndex(0);
        // if(DrawPropertyMiniButton(ICON_FA_COPY)) {
        //     ImGui::SetClipboardText(prim.GetPrimPath().GetString().c_str());
        // }

        // ImGui::TableSetColumnIndex(1);
        // ImGui::Text("%s", prim.GetTypeName().GetString().c_str());
        // ImGui::TableSetColumnIndex(2);
        // {
        //     ScopedStyleColor pathColor(ImGuiCol_Text, GetPrimColor(prim));
        //     ImGui::Text("%s", prim.GetPrimPath().GetString().c_str());
        // }
        
        // ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        // ImGui::TableSetColumnIndex(1);
        // ImGui::Text("Stage");
        // ImGui::TableSetColumnIndex(2);
        // ImGui::Text("%s", prim.GetStage()->GetRootLayer()->GetIdentifier().c_str());
        
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        ImGui::TableSetColumnIndex(0);
        DrawPropertyMiniButton(ICON_FA_CROSSHAIRS);
        ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonLeft;
        if (ImGui::BeginPopupContextItem(nullptr, flags)) {
            DrawUsdPrimEditTarget(prim);
            ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Edit target");
        ImGui::TableSetColumnIndex(2);
        {
            auto targetPath = editTarget.MapToSpecPath(prim.GetPath());
            ScopedStyleColor color(ImGuiCol_Text,
                                   targetPath == SdfPath() ? ImVec4(1.0, 0.0, 0.0, 1.0) : ImVec4(1.0, 1.0, 1.0, 1.0));
            ImGui::Text("%s %s", editTarget.GetLayer()->GetDisplayName().c_str(), targetPath.GetString().c_str());
        }
        
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        ImGui::PushID("Schemas");
        ImGui::TableSetColumnIndex(0);
        // Draw an option button for certain types of prims
        if (prim.IsA<UsdShadeMaterial>()) {
            DrawPropertyMiniButton(ICON_FA_PROJECT_DIAGRAM);
            if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
                if (ImGui::MenuItem("Open in connection editor")) {
                    OpenContainerSheet(prim);
                }
                ImGui::EndPopup();
            }
        }
        ImGui::TableSetColumnIndex(1);

        DrawPrimTypeCombo(prim);
        ImGui::TableSetColumnIndex(2);
        const bool editable = IsPrimEditable(prim);
        ImGui::BeginDisabled(!editable);
        if (DrawPropertyMiniButton(ICON_FA_EDIT)) {
            editSchemas = !editSchemas;
        }
        ImGui::EndDisabled();
        editSchemas = editSchemas && editable;
        //ImGui::Text("Schemas"); TODO Hint
        ImGui::SameLine();
        ImGui::PushItemWidth(-FLT_MIN);
        DrawPrimSchemas(prim, editSchemas);
        ImGui::PopID(); // "Schemas"
        ImGui::EndTable();
    }
}

/// All the children, including inactive, undefined and instance proxies, so the navigation can reach any prim
static Usd_PrimFlagsPredicate NavigatorChildrenPredicate() { return UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate); }

static std::vector<UsdPrim> GetNavigatorChildren(const UsdPrim &prim) {
    const UsdPrimSiblingRange children = prim.GetFilteredChildren(NavigatorChildrenPredicate());
    return std::vector<UsdPrim>(children.begin(), children.end());
}

/// Width fitting the longest name, an auto-resizing popup can't size a child window from its content
static float GetChildrenListWidth(const std::vector<UsdPrim> &children) {
    float width = 0.f;
    for (const UsdPrim &child : children) {
        width = std::max(width, ImGui::CalcTextSize(child.GetName().GetText()).x);
    }
    return width + ImGui::GetStyle().ScrollbarSize + 2 * ImGui::GetStyle().WindowPadding.x;
}

/// Clipped list of children, clicking one selects it
static void DrawChildrenList(const UsdPrim &prim, const std::vector<UsdPrim> &children) {
    constexpr int maxVisibleLines = 15;
    const int visibleLines = std::min(static_cast<int>(children.size()), maxVisibleLines);
    // No spacing after the last line, and child windows have no padding by default
    const float listHeight = visibleLines * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##Children", ImVec2(GetChildrenListWidth(children), listHeight));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(children.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const UsdPrim &child = children[i];
            ScopedStyleColor childColor(ImGuiCol_Text, GetPrimColor(child));
            ImGui::PushID(i);
            if (ImGui::Selectable(child.GetName().GetText())) {
                ExecuteAfterDraw<EditorSetSelection>(prim.GetStage(), child.GetPath());
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

/// Popup listing the children of the prim
static void DrawChildrenPopup(const UsdPrim &prim) {
    if (!ImGui::BeginPopup("##ChildrenPopup")) {
        return;
    }
    {
        // The breadcrumb pushes the prim color and a transparent button, restore the defaults for the popup.
        // Scoped so the colors are popped before EndPopup, ImGui asserts on colors leaking out of a window.
        ScopedStyleColor defaultStyle(DefaultColorStyle);
        DrawChildrenList(prim, GetNavigatorChildren(prim));
    }
    ImGui::EndPopup();
}

/// Last element of the breadcrumb: opens the children popup, or plain text when there is nothing to navigate to
static void DrawNavigatorLastElement(const UsdPrim &prim, const char *label) {
    if (prim.GetFilteredChildren(NavigatorChildrenPredicate()).empty()) {
        ImGui::Text("%s", label);
        return;
    }
    const std::string buttonLabel = std::string(label) + " " ICON_FA_CARET_DOWN;
    if (ImGui::SmallButton(buttonLabel.c_str())) {
        ImGui::OpenPopup("##ChildrenPopup");
    }
    DrawChildrenPopup(prim);
}

/// Path of the prim, each ancestor is a link selecting it, the prim itself opens the list of its children
static void DrawNavigatorBreadcrumb(const UsdPrim &prim) {
    const std::vector<SdfPath> prefixes = prim.GetPrimPath().GetPrefixes();
    ScopedStyleColor buttonStyle(ImGuiCol_Button, ImVec4(ColorTransparent));
    ScopedStyleColor pathColor(ImGuiCol_Text, GetPrimColor(prim));
    ImGui::BeginChild("##Navigation", ImVec2(-FLT_MIN, ImGui::GetFrameHeight()));
    ImGui::AlignTextToFramePadding();
    if (prefixes.empty()) { // pseudo root
        DrawNavigatorLastElement(prim, "/");
        ImGui::EndChild();
        return;
    }
    // The root selects the pseudo root, showing the stage metadata
    if (NavigationLink("/")) {
        ExecuteAfterDraw<EditorSetSelection>(prim.GetStage(), SdfPath::AbsoluteRootPath());
    }
    const int lastIndex = static_cast<int>(prefixes.size()) - 1;
    for (int i = 0; i <= lastIndex; ++i) {
        ImGui::SameLine(0, 0);
        ImGui::PushID(i);
        if (i == lastIndex) {
            DrawNavigatorLastElement(prim, prefixes[i].GetName().c_str());
        } else {
            if (NavigationLink(prefixes[i].GetName().c_str())) {
                ExecuteAfterDraw<EditorSetSelection>(prim.GetStage(), prefixes[i]);
            }
            ImGui::SameLine(0, 0);
            ImGui::Text("/");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void DrawNavigator(UsdPrim &prim) {
    // Navigation
    if (ImGui::BeginTable("##DrawNavigator", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
            ExecuteAfterDraw<EditorSetPreviousPrim>();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ARROW_RIGHT)) {
            ExecuteAfterDraw<EditorSetNextPrim>();
        }

        if (prim) {
            ImGui::TableSetColumnIndex(1);
            ImGui::SameLine();
            DrawNavigatorBreadcrumb(prim);
        }
        ImGui::EndTable();
    }
}

/// Reverse connections of the shading networks: for an attribute, the attributes connected to it.
/// USD only returns the sources of the connections, so the destinations are found by traversing the network scope,
/// which is the outermost node graph (or material) containing the shader. The result is kept until the stage changes.
struct ShadingNetworkConnections : public TfWeakBase {
    const SdfPathVector &GetDestinations(const UsdPrim &shaderPrim, const SdfPath &attributePath) {
        const UsdStageWeakPtr stage = shaderPrim.GetStage();
        if (stage != _stage) {
            TfNotice::Revoke(_noticeKey);
            _stage = stage;
            _noticeKey = TfNotice::Register(TfCreateWeakPtr(this), &ShadingNetworkConnections::OnObjectsChanged, _stage);
            _dirty = true;
        }
        const UsdPrim scope = GetNetworkScope(shaderPrim);
        if (_dirty || scope.GetPath() != _scopePath) {
            Compute(scope);
        }
        static const SdfPathVector noDestinations;
        const auto it = _destinations.find(attributePath);
        return it != _destinations.end() ? it->second : noDestinations;
    }

  private:
    void OnObjectsChanged(const UsdNotice::ObjectsChanged &, const UsdStageWeakPtr &) { _dirty = true; }

    static UsdPrim GetNetworkScope(const UsdPrim &shaderPrim) {
        UsdPrim scope = shaderPrim.GetParent();
        for (UsdPrim prim = scope; prim && !prim.IsPseudoRoot(); prim = prim.GetParent()) {
            if (prim.IsA<UsdShadeNodeGraph>()) { // materials are node graphs
                scope = prim;
            }
        }
        return scope;
    }

    void Compute(const UsdPrim &scope) {
        _destinations.clear();
        _scopePath = scope.GetPath();
        _dirty = false;
        for (const UsdPrim &prim : UsdPrimRange(scope, UsdTraverseInstanceProxies())) {
            for (const UsdAttribute &attribute : prim.GetAttributes()) {
                SdfPathVector sources;
                attribute.GetConnections(&sources);
                for (const SdfPath &source : sources) {
                    _destinations[source].push_back(attribute.GetPath());
                }
            }
        }
    }

    UsdStageWeakPtr _stage;
    TfNotice::Key _noticeKey;
    SdfPath _scopePath;
    bool _dirty = true;
    std::unordered_map<SdfPath, SdfPathVector, SdfPath::Hash> _destinations;
};

// Options are only supported on the value types we can convert from the option strings
static bool HasEditableOptions(SdrShaderPropertyConstPtr property, const VtValue &value) {
    return !property->GetOptions().empty() && (value.IsHolding<TfToken>() || value.IsHolding<std::string>() ||
                                               value.IsHolding<int>() || value.IsHolding<float>());
}

// Combo listing the options of a shader input. An option without value uses its name as value.
static VtValue DrawShaderInputOptions(const std::string &label, const VtValue &value, const std::vector<ShaderOption> &options) {
    auto toValue = [&](const ShaderOption &option) {
        const std::string &str = option.second.IsEmpty() ? option.first.GetString() : option.second.GetString();
        if (value.IsHolding<TfToken>())
            return VtValue(TfToken(str));
        if (value.IsHolding<int>())
            return VtValue(static_cast<int>(std::strtol(str.c_str(), nullptr, 10)));
        if (value.IsHolding<float>())
            return VtValue(std::strtof(str.c_str(), nullptr));
        return VtValue(str);
    };
    std::string preview = TfStringify(value);
    for (const ShaderOption &option : options) {
        if (toValue(option) == value) {
            preview = option.first.GetString();
            break;
        }
    }
    VtValue modified;
    if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
        for (const ShaderOption &option : options) {
            const VtValue optionValue = toValue(option);
            if (ImGui::Selectable(option.first.GetText(), optionValue == value)) {
                modified = optionValue;
            }
        }
        ImGui::EndCombo();
    }
    return modified;
}

// attribute is invalid when the input is not on the prim, the value is then the definition default
static VtValue DrawShaderInputValue(const std::string &label, UsdAttribute &attribute, SdrShaderPropertyConstPtr property,
                                    const SdfValueTypeName &type, const VtValue &value) {
    if (HasEditableOptions(property, value)) {
        return DrawShaderInputOptions(label, value, property->GetOptions());
    }
    if (attribute) {
        return DrawAttributeValue(label, attribute, value);
    }
    if (type.GetRole() == SdfValueRoleNames->Color) {
        return DrawColorValue(label, value);
    }
    return DrawVtValue(label, value);
}

// Creates the input or output on the prim and sets its value if any, in a single undoable command
static void CreateShaderAttribute(const UsdPrim &prim, const TfToken &attributeName, const SdfValueTypeName &type,
                              const VtValue &value) {
    const UsdStageWeakPtr stage = prim.GetStage();
    const SdfPath primPath = prim.GetPath();
    std::function<void()> createAttribute = [=]() {
        if (UsdPrim prim = stage ? stage->GetPrimAtPath(primPath) : UsdPrim()) {
            UsdAttribute attribute = prim.CreateAttribute(attributeName, type, false /* custom */);
            if (attribute && !value.IsEmpty()) {
                attribute.Set(value);
            }
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(TfCreateRefPtrFromProtectedWeakPtr(stage->GetEditTarget().GetLayer()), createAttribute);
}

// Name of a shader property, the label of the definition if any, with its help in the tooltip
static void DrawShaderPropertyName(const TfToken &name, SdrShaderPropertyConstPtr property, const TfToken &attributeName,
                                   const SdfValueTypeName &type, bool authored) {
    const TfToken &label = property->GetLabel().IsEmpty() ? name : property->GetLabel();
    ImGui::TextColored(authored ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored), "%s", label.GetText());
    if (IsItemHoveredForHint()) {
        const std::string help = property->GetHelp();
        ImGui::SetTooltip("%s (%s)%s%s", attributeName.GetText(), type.GetAsToken().GetText(), help.empty() ? "" : "\n",
                          help.c_str());
    }
}

static void DrawShaderInputRow(const UsdPrim &prim, const TfToken &name, SdrShaderPropertyConstPtr property,
                               const UsdEditTarget &editTarget, UsdTimeCode currentTime, bool editable,
                               const OpinionStrength &editTargetStrength) {
    const TfToken attributeName(UsdShadeTokens->inputs.GetString() + name.GetString());
    const SdfValueTypeName type = GetShaderPropertySdfType(property);
    UsdAttribute attribute = prim.GetAttribute(attributeName);
    ImGui::PushID(attributeName.GetText());
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);

    // The value of the attribute if it has one, otherwise the default of the definition
    VtValue value;
    const bool hasValue = attribute && attribute.Get(&value, currentTime);
    if (!hasValue) {
        value = property->GetDefaultValueAsSdfType();
        if (value.IsEmpty()) {
            value = type.GetDefaultValue();
        }
    }

    ImGui::TableSetColumnIndex(0);
    if (attribute) {
        DrawPropertyMiniButton(attribute, editTarget, currentTime, editable);
    } else {
        DrawPropertyMiniButton(SmallButtonLabel<UsdAttribute>());
        if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
            if (ImGui::MenuItem(ICON_FA_PLUS_CIRCLE " Create value", nullptr, false, editable && !value.IsEmpty())) {
                CreateShaderAttribute(prim, attributeName, type, value);
            }
            ImGui::EndPopup();
        }
    }

    ImGui::TableSetColumnIndex(1);
    DrawShaderPropertyName(name, property, attributeName, type, attribute && attribute.IsAuthored());

    ImGui::TableSetColumnIndex(2);
    ImGui::PushItemWidth(-FLT_MIN);
    const std::string shadowedReason =
        attribute ? GetShadowedValueReason(attribute, currentTime, editTargetStrength) : std::string();
    if (!value.IsEmpty()) {
        VtValue modified;
        {
            const ImVec4 valueColor = !shadowedReason.empty() ? ImVec4(ColorAttributeShadowed)
                                      : hasValue              ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                              : ImVec4(ColorAttributeUnauthored);
            ScopedStyleColor defaultValueColor(ImGuiCol_Text, valueColor);
            ImGui::BeginDisabled(!editable);
            modified = DrawShaderInputValue(attributeName.GetString(), attribute, property, type, value);
            ImGui::EndDisabled();
        }
        if (!modified.IsEmpty()) {
            if (attribute) {
                ExecuteAfterDraw<AttributeSet>(attribute, modified,
                                               attribute.GetNumTimeSamples() ? currentTime : UsdTimeCode::Default());
            } else {
                CreateShaderAttribute(prim, attributeName, type, modified);
            }
        }
    } else {
        ImGui::TextColored(ImVec4(ColorAttributeUnauthored), "no value");
    }
    if (attribute && attribute.HasAuthoredConnections()) {
        DrawAttributeConnections(attribute, editable);
    }
    DrawShadowedValueTooltip(shadowedReason);
    ImGui::PopItemWidth();
    ImGui::PopID();
}

static void DrawShaderOutputRow(const UsdPrim &prim, const TfToken &name, SdrShaderPropertyConstPtr property,
                                const UsdEditTarget &editTarget, UsdTimeCode currentTime,
                                ShadingNetworkConnections &connections, bool editable) {
    const TfToken attributeName(UsdShadeTokens->outputs.GetString() + name.GetString());
    const SdfValueTypeName type = GetShaderPropertySdfType(property);
    UsdAttribute attribute = prim.GetAttribute(attributeName);
    ImGui::PushID(attributeName.GetText());
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
    ImGui::TableSetColumnIndex(0);
    if (attribute) {
        DrawPropertyMiniButton(attribute, editTarget, currentTime, editable);
    } else {
        DrawPropertyMiniButton(SmallButtonLabel<UsdAttribute>());
        if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
            if (ImGui::MenuItem(ICON_FA_PLUS_CIRCLE " Create output", nullptr, false, editable)) {
                CreateShaderAttribute(prim, attributeName, type, VtValue());
            }
            ImGui::EndPopup();
        }
    }
    ImGui::TableSetColumnIndex(1);
    DrawShaderPropertyName(name, property, attributeName, type, attribute && attribute.IsAuthored());
    ImGui::TableSetColumnIndex(2);
    for (const SdfPath &destination : connections.GetDestinations(prim, prim.GetPath().AppendProperty(attributeName))) {
        ImGui::PushID(destination.GetString().c_str());
        DrawConnectionLink(prim.GetStage(), destination);
        ImGui::PopID();
    }
    ImGui::PopID();
}

/// Inputs and outputs of the shader definition, authored or not, the inputs grouped by page.
/// Returns true if the prim is a shader.
static bool DrawShaderParameters(const UsdPrim &prim, UsdTimeCode currentTime) {
    if (!prim.IsA<UsdShadeShader>())
        return false;
    SdrShaderNodeConstPtr shaderNode = GetShaderDefinition(prim);
    if (!shaderNode) {
        ImGui::TextColored(ImVec4(ColorAttributeUnauthored), "Shader definition not found");
        return true;
    }
    static ShadingNetworkConnections connections;
    const UsdEditTarget &editTarget = prim.GetStage()->GetEditTarget();
    const bool editable = IsPrimEditable(prim);
    const OpinionStrength editTargetStrength = editable ? GetEditTargetStrength(prim) : OpinionStrength();

    // Pages in the order of their first input, the inputs without page come first
    std::vector<std::pair<TfToken, std::vector<TfToken>>> pages = {{TfToken(), {}}};
    for (const TfToken &name : GetShaderDefinitionInputNames(shaderNode)) {
        if (SdrShaderPropertyConstPtr property = shaderNode->GetShaderInput(name)) {
            auto page = std::find_if(pages.begin(), pages.end(), [&](const auto &p) { return p.first == property->GetPage(); });
            if (page == pages.end()) {
                page = pages.emplace(pages.end(), property->GetPage(), std::vector<TfToken>());
            }
            page->second.push_back(name);
        }
    }

    if (ImGui::BeginTable("##DrawShaderParameters", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("Shader parameter");
        ImGui::TableSetupColumn(shaderNode->GetIdentifier().GetText(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto &[page, names] : pages) {
            bool pageOpen = true;
            if (!page.IsEmpty()) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
                ImGui::TableSetColumnIndex(1);
                const bool openByDefault = IsShaderDefinitionPageOpen(shaderNode, page);
                pageOpen = ImGui::TreeNodeEx(page.GetText(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                 (openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0));
            }
            if (pageOpen) {
                for (const TfToken &name : names) {
                    DrawShaderInputRow(prim, name, shaderNode->GetShaderInput(name), editTarget, currentTime, editable,
                                       editTargetStrength);
                }
            }
        }
        for (const TfToken &name : GetShaderDefinitionOutputNames(shaderNode)) {
            if (SdrShaderPropertyConstPtr property = shaderNode->GetShaderOutput(name)) {
                DrawShaderOutputRow(prim, name, property, editTarget, currentTime, connections, editable);
            }
        }
        ImGui::EndTable();
    }
    return true;
}

//
// Stage metadata, shown when the pseudo root is selected
//

/// Why the stage metadata can't be edited with the current edit target, empty when it can
static std::string GetStageMetadataBlockedReason(const UsdStageWeakPtr &stage) {
    const SdfLayerHandle editLayer = stage->GetEditTarget().GetLayer();
    if (!editLayer) {
        return "The stage has no edit target.\nChoose the root or session layer with the edit target button.";
    }
    if (editLayer != stage->GetRootLayer() && editLayer != stage->GetSessionLayer()) {
        return "USD reads the stage metadata from the root and session layers only, the edit target " +
               editLayer->GetDisplayName() + " is neither.\nChoose one of them with the edit target button.";
    }
    if (!editLayer->PermissionToEdit()) {
        return "The edit target " + editLayer->GetDisplayName() + " is read-only.";
    }
    return {};
}

static void DrawStageEditTargetMenuItems(const UsdStageWeakPtr &stage) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    const SdfLayerHandle editLayer = stage->GetEditTarget().GetLayer();
    if (ImGui::MenuItem("Root layer", nullptr, editLayer == stage->GetRootLayer())) {
        ExecuteAfterDraw<EditorSetEditTarget>(stage, UsdEditTarget(stage->GetRootLayer()));
    }
    if (ImGui::MenuItem("Session layer", nullptr, editLayer == stage->GetSessionLayer())) {
        ExecuteAfterDraw<EditorSetEditTarget>(stage, UsdEditTarget(stage->GetSessionLayer()));
    }
}

static void DrawStageHeader(const UsdStageWeakPtr &stage) {
    if (ImGui::BeginTable("##DrawStageHeader", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        ImGui::TableSetColumnIndex(0);
        DrawPropertyMiniButton(ICON_FA_CROSSHAIRS);
        if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
            DrawStageEditTargetMenuItems(stage);
            ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Edit target");
        ImGui::TableSetColumnIndex(2);
        const SdfLayerHandle editLayer = stage->GetEditTarget().GetLayer();
        ImGui::Text("%s", editLayer ? editLayer->GetDisplayName().c_str() : "none");
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Stage");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", stage->GetRootLayer()->GetIdentifier().c_str());
        ImGui::EndTable();
    }
}

static void DrawStageProperties(const UsdStageWeakPtr &stage) {
    const std::string blockedReason = GetStageMetadataBlockedReason(stage);
    ImVec2 headerSize(-FLT_MIN, ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().CellPadding.y * 2.f);
    if (!blockedReason.empty()) {
        headerSize.y += ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight();
    }
    ImGui::BeginChild("##Header", headerSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawStageHeader(stage);
    DrawEditBlockedBanner(blockedReason);
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginChild("##Body");
    ImGui::BeginDisabled(!blockedReason.empty());
    DrawStageMetadata(stage);
    ImGui::EndDisabled();
    DrawEditBlockedTooltip(blockedReason);
    ImGui::EndChild();
}

//
// Instancing section
//

/// The closest prim, starting from the prim itself, passing the test
static UsdPrim FindSelfOrAncestor(const UsdPrim &prim, bool (UsdPrim::*test)() const) {
    for (UsdPrim current = prim; current && !current.IsPseudoRoot(); current = current.GetParent()) {
        if ((current.*test)()) {
            return current;
        }
    }
    return {};
}

static const char *GetArcTypeName(PcpArcType arcType) {
    switch (arcType) {
    case PcpArcTypeReference:
        return "reference";
    case PcpArcTypePayload:
        return "payload";
    case PcpArcTypeInherit:
        return "inherit";
    case PcpArcTypeSpecialize:
        return "specialize";
    case PcpArcTypeVariant:
        return "variant";
    case PcpArcTypeRelocate:
        return "relocate";
    default:
        return "arc";
    }
}

/// The strongest layer of the node layer stack having a spec at the path, the root layer of the stack when none has
static SdfLayerHandle FindLayerWithSpec(const PcpNodeRef &node, const SdfPath &path) {
    for (const SdfLayerRefPtr &layer : node.GetLayerStack()->GetLayers()) {
        if (layer->HasSpec(path)) {
            return layer;
        }
    }
    return node.GetLayerStack()->GetIdentifier().rootLayer;
}

static void BeginInstancingRow(const char *label) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", label);
    ImGui::TableSetColumnIndex(2);
}

static void DrawSelectPrimLink(const UsdPrim &prim, const SdfPath &path) {
    if (NavigationLink(path.GetText())) {
        ExecuteAfterDraw<EditorSetSelection>(prim.GetStage(), path);
    }
}

/// Turns off instanceable on the instance, its children become regular prims that can be edited
static void DrawMakeInstanceEditableRow(const UsdPrim &instance) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
    ImGui::TableSetColumnIndex(2);
    const std::string blockedReason = GetEditBlockedReason(instance);
    ImGui::BeginDisabled(!blockedReason.empty());
    if (ImGui::Button("Make this instance editable")) {
        ExecuteAfterDraw(&UsdPrim::SetInstanceable, instance, false);
    }
    ImGui::EndDisabled();
    if (IsItemHoveredForHint(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", blockedReason.empty() ? ("Sets instanceable to false on " + instance.GetPath().GetString() +
                                                         " in the edit target.\nIts prims stop sharing the prototype and can be edited.")
                                                            .c_str()
                                                      : blockedReason.c_str());
    }
}

/// Composition arcs of an instance, the content shared by all the instances of the prototype comes from them.
/// The links open the layer at the path matching the prim, relative to the prototype.
static void DrawPrototypeOrigin(const UsdPrim &instance, const SdfPath &relativePath) {
    const PcpNodeRef root = instance.GetPrimIndex().GetRootNode();
    int linkId = 0;
    TF_FOR_ALL(child, root.GetChildrenRange()) {
        const PcpNodeRef node = *child;
        if (node.IsInert() || node.IsCulled()) {
            continue;
        }
        const SdfPath path = relativePath.IsEmpty() ? node.GetPath() : node.GetPath().AppendPath(relativePath);
        const SdfLayerHandle layer = FindLayerWithSpec(node, path);
        if (!layer) {
            continue;
        }
        if (linkId > 0) {
            BeginInstancingRow("");
        }
        ImGui::PushID(linkId++);
        ImGui::Text("%s", GetArcTypeName(node.GetArcType()));
        ImGui::SameLine();
        const std::string label = layer->GetDisplayName() + " " + path.GetString();
        if (NavigationLink(label.c_str())) {
            ExecuteAfterDraw<EditorSetSelection>(layer, path);
        }
        if (IsItemHoveredForHint()) {
            ImGui::SetTooltip("%s", layer->GetIdentifier().c_str());
        }
        ImGui::PopID();
    }
    if (linkId == 0) {
        ImGui::TextUnformatted("no arc found");
    }
}

/// The path of the prim matching the prototype prim in an instance, relativePath is empty for the prototype itself
static SdfPath GetPathInInstance(const UsdPrim &instance, const SdfPath &relativePath) {
    return relativePath.IsEmpty() ? instance.GetPath() : instance.GetPath().AppendPath(relativePath);
}

/// The paths of the prims matching the prototype prim in each instance
static SdfPathVector GetPathsInInstances(const std::vector<UsdPrim> &instances, const SdfPath &relativePath) {
    SdfPathVector paths;
    paths.reserve(instances.size());
    for (const UsdPrim &instance : instances) {
        paths.push_back(GetPathInInstance(instance, relativePath));
    }
    return paths;
}

/// Links to the matching prims in the instances, clipped as there can be thousands: the paths are only built for the
/// visible lines, and for all the instances when "Select all" is clicked
static void DrawInstancesList(const UsdPrim &prim, const std::vector<UsdPrim> &instances, const SdfPath &relativePath) {
    if (ImGui::SmallButton("Select all")) {
        ExecuteAfterDraw<EditorSetSelection>(prim.GetStage(), GetPathsInInstances(instances, relativePath));
    }
    const int nbInstances = static_cast<int>(instances.size());
    constexpr int maxVisibleLines = 8;
    const int visibleLines = std::min(nbInstances, maxVisibleLines);
    const float height = ImGui::GetTextLineHeightWithSpacing() * visibleLines + ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("##Instances", ImVec2(-FLT_MIN, height));
    ImGuiListClipper clipper;
    clipper.Begin(nbInstances);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ImGui::PushID(i);
            DrawSelectPrimLink(prim, GetPathInInstance(instances[i], relativePath));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

static void DrawInstanceRows(const UsdPrim &instance) {
    const UsdPrim prototype = instance.GetPrototype();
    BeginInstancingRow("Prototype");
    DrawSelectPrimLink(instance, prototype.GetPath());
    BeginInstancingRow("Shared with");
    ImGui::Text("%zu other instances", prototype.GetInstances().size() - 1);
    DrawMakeInstanceEditableRow(instance);
}

static void DrawInstanceProxyRows(const UsdPrim &proxy) {
    const UsdPrim instance = FindSelfOrAncestor(proxy, &UsdPrim::IsInstance);
    BeginInstancingRow("Instance proxy of");
    DrawSelectPrimLink(proxy, proxy.GetPrimInPrototype().GetPath());
    if (instance) {
        BeginInstancingRow("Inside instance");
        DrawSelectPrimLink(proxy, instance.GetPath());
        DrawMakeInstanceEditableRow(instance);
    }
}

static void DrawPrototypeRows(const UsdPrim &prim) {
    const UsdPrim prototype = FindSelfOrAncestor(prim, &UsdPrim::IsPrototype);
    if (!prototype) {
        return;
    }
    // Empty for the prototype itself
    const SdfPath relativePath = prim == prototype ? SdfPath() : prim.GetPath().MakeRelativePath(prototype.GetPath());
    if (prim != prototype) {
        BeginInstancingRow("In prototype");
        DrawSelectPrimLink(prim, prototype.GetPath());
    }
    const std::vector<UsdPrim> instances = prototype.GetInstances();
    if (instances.empty()) {
        return;
    }
    // All the instances share the same arcs, that's why they share the prototype
    BeginInstancingRow("Comes from");
    DrawPrototypeOrigin(instances.front(), relativePath);
    const std::string instancesLabel = "Instances (" + std::to_string(instances.size()) + ")";
    BeginInstancingRow(instancesLabel.c_str());
    DrawInstancesList(prim, instances, relativePath);
}

/// Where the content of an instanced prim comes from and where it's used, with the ways to edit it.
/// Returns true when the prim is involved in instancing.
static bool DrawInstancing(const UsdPrim &prim) {
    if (!prim.IsInstance() && !prim.IsInstanceProxy() && !prim.IsPrototype() && !prim.IsInPrototype()) {
        return false;
    }
    if (ImGui::BeginTable("##DrawInstancing", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
        ImGui::TableSetupColumn("Instancing");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        if (prim.IsInstance()) {
            DrawInstanceRows(prim);
        } else if (prim.IsInstanceProxy()) {
            DrawInstanceProxyRows(prim);
        } else {
            DrawPrototypeRows(prim);
        }
        ImGui::EndTable();
    }
    return true;
}

void DrawUsdPrimProperties(UsdPrim &prim, UsdTimeCode currentTime) {

    DrawPropertyEditorMenuBar();
    DrawNavigator(prim);
    if (prim && prim.IsPseudoRoot()) {
        DrawStageProperties(prim.GetStage());
        return;
    }
    if (prim) {
        const std::string editBlockedReason = GetEditBlockedReason(prim);
        auto headerSize = ImGui::GetWindowSize();
        // 2 rows, the second one holds the type combo which adds the cell padding to the frame height
        headerSize.y = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().CellPadding.y * 2.f;
        // The edit blocked banner stays in the header, visible while the body scrolls
        if (!editBlockedReason.empty()) {
            headerSize.y += ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight();
        }
        headerSize.x = -FLT_MIN; // expand as much as possible
        ImGui::BeginChild("##Header", headerSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawUsdPrimHeader(prim);
        DrawEditBlockedBanner(editBlockedReason);
        DrawEditBlockedTooltip(editBlockedReason);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::BeginChild("##Body");
        
        DrawSection(PrimEditorSection::Instancing, [&]() { return DrawInstancing(prim); });
        DrawSection(PrimEditorSection::ShaderInputs, [&]() { return DrawShaderParameters(prim, currentTime); });
        DrawSection(PrimEditorSection::Metadata, [&]() { return DrawPrimMetadata(prim); });
        DrawSection(PrimEditorSection::AssetInfo, [&]() { return DrawAssetInfo(prim); });
        DrawSection(PrimEditorSection::MaterialBindings, [&]() { return DrawMaterialBindings(prim); });
        DrawSection(PrimEditorSection::Variants, [&]() { return DrawVariantSetsCombos(prim); });
        DrawSection(PrimEditorSection::Transform, [&]() { return DrawXformsCommon(prim, currentTime); });

        const bool showAttributes = IsSectionShown(PrimEditorSection::Attributes);
        const bool showRelationships = IsSectionShown(PrimEditorSection::Relationships);
        if ((showAttributes || showRelationships) &&
            ImGui::BeginTable("##DrawPropertyEditorTable", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, GetMiniButtonSize());
            ImGui::TableSetupColumn("Property name");
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            int miniButtonId = 0;
            const auto &editTarget = prim.GetStage()->GetEditTarget();
            const bool editable = IsPrimEditable(prim);
            const OpinionStrength editTargetStrength = editable ? GetEditTargetStrength(prim) : OpinionStrength();

            // Draw attributes
            std::vector<UsdAttribute> attributes = showAttributes ? prim.GetAttributes() : std::vector<UsdAttribute>();
            for (auto &attribute : attributes) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowMinHeight);
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(miniButtonId++);
                DrawPropertyMiniButton(attribute, editTarget, currentTime, editable);
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                DrawAttributeDisplayName(attribute);

                ImGui::TableSetColumnIndex(2);
                ImGui::PushItemWidth(-FLT_MIN); // Right align and get rid of widget label
                ImGui::PushID(attribute.GetPath().GetHash());
                const std::string shadowedReason = GetShadowedValueReason(attribute, currentTime, editTargetStrength);
                {
                    const ImVec4 valueColor =
                        shadowedReason.empty() ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImVec4(ColorAttributeShadowed);
                    ScopedStyleColor shadowedColor(ImGuiCol_Text, valueColor);
                    DrawAttributeValueAtTime(attribute, currentTime, editable);
                }
                DrawShadowedValueTooltip(shadowedReason);
                ImGui::PopID();
                ImGui::PopItemWidth();
                // TODO: in the hint ???
                // DrawAttributeTypeInfo(attribute);
            }

            // Draw relations
            std::vector<UsdRelationship> relationships = showRelationships ? prim.GetRelationships() : std::vector<UsdRelationship>();
            for (auto &relationship : relationships) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(miniButtonId++);
                DrawPropertyMiniButton(relationship, editTarget, currentTime, editable);
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                DrawUsdRelationshipDisplayName(relationship);

                ImGui::TableSetColumnIndex(2);
                DrawUsdRelationshipList(relationship, editable);
            }

            ImGui::EndTable();
        }
        DrawEditBlockedTooltip(editBlockedReason);
        ImGui::EndChild();
    }
}


#define GENERATE_XFORMCOMMON_FIELD(OpName_, OpType_, OpRawType_)                                                                 \
    struct XformCommon##OpName_##Field {                                                                                         \
        static constexpr const char *fieldName = "" #OpName_ "";                                                                 \
    };                                                                                                                           \
    template <>                                                                                                                  \
    inline void DrawThirdColumn<XformCommon##OpName_##Field>(const int rowId, const UsdGeomXformCommonAPI &xformAPI,             \
                                                            const OpType_ &value, const UsdTimeCode &currentTime) {              \
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);                                                               \
        OpType_ valueLocal(value[0], value[1], value[2]);                                                                        \
        ImGui::InputScalarN("" #OpName_ "", OpRawType_, valueLocal.data(), 3, nullptr, nullptr, DecimalPrecision);               \
        if (ImGui::IsItemDeactivatedAfterEdit()) {                                                                               \
            ExecuteAfterDraw(&UsdGeomXformCommonAPI::Set##OpName_, xformAPI, valueLocal, currentTime);                           \
        }                                                                                                                        \
    }

GENERATE_XFORMCOMMON_FIELD(Translate, GfVec3d, ImGuiDataType_Double);
GENERATE_XFORMCOMMON_FIELD(Scale, GfVec3f, ImGuiDataType_Float);
GENERATE_XFORMCOMMON_FIELD(Pivot, GfVec3f, ImGuiDataType_Float);

struct XformCommonRotateField {
    static constexpr const char *fieldName = "Rotate";
};
template <>
inline void DrawThirdColumn<XformCommonRotateField>(const int rowId, const UsdGeomXformCommonAPI &xformAPI,
                                                   const GfVec3f &rotation, const UsdGeomXformCommonAPI::RotationOrder &rotOrder,
                                                   const UsdTimeCode &currentTime) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    GfVec3f rotationf(rotation[0], rotation[1], rotation[2]);
    ImGui::InputScalarN("Rotate", ImGuiDataType_Float, rotationf.data(), 3, nullptr, nullptr, DecimalPrecision);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw(&UsdGeomXformCommonAPI::SetRotate, xformAPI, rotationf, rotOrder, currentTime);
    }
}

// Draw a xform common api in a table
// I am not sure this is really useful
bool DrawXformsCommon(UsdPrim &prim, UsdTimeCode currentTime) {

    UsdGeomXformCommonAPI xformAPI(prim);

    if (xformAPI) {
        GfVec3d translation;
        GfVec3f scale;
        GfVec3f pivot;
        GfVec3f rotation;
        UsdGeomXformCommonAPI::RotationOrder rotOrder;
        xformAPI.GetXformVectors(&translation, &rotation, &scale, &pivot, &rotOrder, currentTime);

        int rowId = 0;
        if (BeginThreeColumnsTable("##DrawXformsCommon")) {
            SetupThreeColumnsTable(true, "", "UsdGeomXformCommonAPI", "");
            ImGui::BeginDisabled(!IsPrimEditable(prim));
            DrawThreeColumnsRow<XformCommonTranslateField>(rowId++, xformAPI, translation, currentTime);
            DrawThreeColumnsRow<XformCommonRotateField>(rowId++, xformAPI, rotation, rotOrder, currentTime);
            DrawThreeColumnsRow<XformCommonScaleField>(rowId++, xformAPI, scale, currentTime);
            DrawThreeColumnsRow<XformCommonPivotField>(rowId++, xformAPI, pivot, currentTime);
            ImGui::EndDisabled();
            EndThreeColumnsTable();
        }
        return true;
    }
    return false;
}
