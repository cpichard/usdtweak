#include <iostream>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/layerStack.h>
#include "Gui.h"
#include "PropertyEditor.h"
#include "ValueEditor.h"
#include "Constants.h"
#include "Commands.h"
#include "ModalDialogs.h"
#include "ImGuiHelpers.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Very basic ui to create a connection
struct CreateConnectionDialog : public ModalDialog {

    CreateConnectionDialog(const UsdAttribute &attribute) : _attribute(attribute){};
    ~CreateConnectionDialog() override {}

    void Draw() override {
        ImGui::Text("Create connection for %s", _attribute.GetPath().GetString().c_str());
        ImGui::InputText("Path", &_connectionEndPoint);
        DrawOkCancelModal([&]() {
            ExecuteAfterDraw(&UsdAttribute::AddConnection, _attribute, SdfPath(_connectionEndPoint),
                             UsdListPositionBackOfPrependList);
        });
    }
    const char *DialogId() const override { return "Attribute connection"; }

    UsdAttribute _attribute;
    std::string _connectionEndPoint;
};

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

void DrawAttributeValueAtTime(UsdAttribute &attribute, UsdTimeCode currentTime) {
    const std::string attributeLabel = GetDisplayName(attribute);
    VtValue value;
    const bool HasValue = attribute.Get(&value, currentTime);

    if (HasValue) {
        VtValue modified = DrawAttributeValue(attributeLabel, attribute, value);
        if (!modified.IsEmpty()) {
            ExecuteAfterDraw<AttributeSet>(attribute, modified,
                                           attribute.GetNumTimeSamples() ? currentTime : UsdTimeCode::Default());
        }
    }

    const bool HasConnections = attribute.HasAuthoredConnections();
    if (HasConnections) {
        SdfPathVector sources;
        attribute.GetConnections(&sources);
        for (auto &connection : sources) {
            ImGui::PushID(connection.GetString().c_str());
            if (ImGui::Button(ICON_FA_TRASH)) {
                ExecuteAfterDraw(&UsdAttribute::RemoveConnection, attribute, connection);
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(ColorAttributeConnection), ICON_FA_LINK " %s", connection.GetString().c_str());
            ImGui::PopID();
        }
    }

    if (!HasValue && !HasConnections) {
        ImGui::TextColored(ImVec4({0.5, 0.5, 0.5, 0.5}), "no value");
    }
}

void DrawUsdRelationshipDisplayName(const UsdRelationship &relationship) {
    std::string relationshipName = GetDisplayName(relationship);
    ImVec4 attributeNameColor = relationship.IsAuthored() ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored);
    ImGui::TextColored(ImVec4(attributeNameColor), "%s", relationshipName.c_str());
}

void DrawUsdRelationshipList(const UsdRelationship &relationship) {
    SdfPathVector targets;
    // relationship.GetForwardedTargets(&targets);
    // for (const auto &path : targets) {
    //    ImGui::TextColored(ImVec4(AttributeRelationshipColor), "%s", path.GetString().c_str());
    //}
    relationship.GetTargets(&targets);
    if (!targets.empty()) {
        if (ImGui::BeginListBox("##Relationship", ImVec2(-FLT_MIN, targets.size() * 25))) {
            for (const auto &path : targets) {
                ImGui::PushID(path.GetString().c_str());
                if (ImGui::Button(ICON_FA_TRASH)) {
                    ExecuteAfterDraw(&UsdRelationship::RemoveTarget, relationship, path);
                }
                ImGui::SameLine();
                std::string buffer = path.GetString();
                ImGui::InputText("##EditRelation", &buffer);
                ImGui::PopID();
            }
            ImGui::EndListBox();
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
        if (ImGui::MenuItem(ICON_FA_EJECT " Clear values")) {
            ExecuteAfterDraw(&UsdAttribute::Clear, attribute);
        }
    }
}

template <typename UsdPropertyT> void DrawMenuBlockValues(UsdPropertyT &property){};
template <> void DrawMenuBlockValues(UsdAttribute &attribute) {
    if (ImGui::MenuItem(ICON_FA_STOP " Block values")) {
        ExecuteAfterDraw(&UsdAttribute::Block, attribute);
    }
}

template <typename UsdPropertyT> void DrawMenuRemoveProperty(UsdPropertyT &property){};
template <> void DrawMenuRemoveProperty(UsdAttribute &attribute) {
    if (ImGui::MenuItem(ICON_FA_TRASH " Remove property")) {
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
static void DrawPropertyMiniButton(const char *btnStr, const ImVec4 &btnColor = ImVec4(ColorMiniButtonUnauthored)) {
    ScopedStyleColor miniButtonStyle(ImGuiCol_Text, btnColor, ImGuiCol_Button, ImVec4(ColorTransparent) );
    ImGui::SmallButton(btnStr);
}

template <typename UsdPropertyT> void DrawMenuEditConnection(UsdPropertyT &property) {}
template <> void DrawMenuEditConnection(UsdAttribute &attribute) {
    if (ImGui::MenuItem(ICON_FA_LINK " Create connection")) {
        DrawModalDialog<CreateConnectionDialog>(attribute);
    }
}

// TODO: relationship
template <typename UsdPropertyT> void DrawMenuCreateValue(UsdPropertyT &property){};

template <> void DrawMenuCreateValue(UsdAttribute &attribute) {
    if (!attribute.HasValue()) {
        if (ImGui::MenuItem(ICON_FA_DONATE " Create value")) {
            ExecuteAfterDraw<AttributeCreateDefaultValue>(attribute);
        }
    }
}

// Property mini button, should work with UsdProperty, UsdAttribute and UsdRelationShip
template <typename UsdPropertyT>
void DrawPropertyMiniButton(UsdPropertyT &property, const UsdEditTarget &editTarget, UsdTimeCode currentTime) {
    ImVec4 propertyColor =
        property.IsAuthoredAt(editTarget) ? ImVec4(ColorMiniButtonAuthored) : ImVec4(ColorMiniButtonUnauthored);
    DrawPropertyMiniButton(SmallButtonLabel<UsdPropertyT>(), propertyColor);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        DrawMenuSetKey(property, currentTime);
        DrawMenuCreateValue(property);
        DrawMenuClearAuthoredValues(property);
        DrawMenuBlockValues(property);
        DrawMenuRemoveProperty(property);
        DrawMenuEditConnection(property);
        if (ImGui::MenuItem(ICON_FA_COPY " Copy attribute path")) {
            ImGui::SetClipboardText(property.GetPath().GetString().c_str());
        }
        ImGui::EndPopup();
    }
}

bool DrawVariantSetsCombos(UsdPrim &prim) {
    int buttonID = 0;
    if (!prim.HasVariantSets())
        return false;
    auto variantSets = prim.GetVariantSets();

    if (ImGui::BeginTable("##DrawVariantSetsCombos", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("VariantSet");
        ImGui::TableSetupColumn("");

        ImGui::TableHeadersRow();

        for (auto variantSetName : variantSets.GetNames()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // Variant set mini button --- TODO move code from this function
            auto variantSet = variantSets.GetVariantSet(variantSetName);
            // TODO: how do we figure out if the variant set has been edited in this edit target ?
            // Otherwise after a "Clear variant selection" the button remains green and it visually looks like it did nothing
            ImVec4 variantColor =
                variantSet.HasAuthoredVariantSelection() ? ImVec4(ColorMiniButtonAuthored) : ImVec4(ColorMiniButtonUnauthored);
            ImGui::PushID(buttonID++);
            DrawPropertyMiniButton("(v)", variantColor);
            ImGui::PopID();
            if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
                if (ImGui::MenuItem("Clear variant selection")) {
                    ExecuteAfterDraw(&UsdVariantSet::ClearVariantSelection, variantSet);
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", variantSetName.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(variantSetName.c_str(), variantSet.GetVariantSelection().c_str())) {
                for (auto variant : variantSet.GetVariantNames()) {
                    if (ImGui::Selectable(variant.c_str(), false)) {
                        ExecuteAfterDraw(&UsdVariantSet::SetVariantSelection, variantSet, variant);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
        }
        ImGui::EndTable();
    }
    return true;
}

bool DrawAssetInfo(UsdPrim &prim) {
    auto assetInfo = prim.GetAssetInfo();
    if (assetInfo.empty())
        return false;

    if (ImGui::BeginTable("##DrawAssetInfo", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Asset info");
        ImGui::TableSetupColumn("");

        ImGui::TableHeadersRow();

        TF_FOR_ALL(keyValue, assetInfo) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", keyValue->first.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            VtValue modified = DrawVtValue(keyValue->first, keyValue->second);
            if (!modified.IsEmpty()) {
                ExecuteAfterDraw(&UsdPrim::SetAssetInfoByKey, prim, TfToken(keyValue->first), modified);
            }
            ImGui::PopItemWidth();
        }
        ImGui::EndTable();
    }
    return true;
}

// WIP
bool IsMetadataShown(int options) { return true; }
bool IsTransformShown(int options) { return false; }

void DrawPropertyEditorMenuBar(UsdPrim &prim, int options) {

    if (ImGui::BeginMenuBar()) {
        if (prim && ImGui::BeginMenu("Create")) {
            // TODO: list all the attribute missing or incomplete
            if (ImGui::MenuItem("Attribute", nullptr)) {
            }
            if (ImGui::MenuItem("Reference", nullptr)) {
            }
            if (ImGui::MenuItem("Relation", nullptr)) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Show")) {
            if (ImGui::MenuItem("Metadata", nullptr, IsMetadataShown(options))) {
            }
            if (ImGui::MenuItem("Transform", nullptr, IsTransformShown(options))) {
            }
            if (ImGui::MenuItem("Attributes", nullptr, false)) {
            }
            if (ImGui::MenuItem("Relations", nullptr, false)) {
            }
            if (ImGui::MenuItem("Composition", nullptr, false)) {
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

// First version of an edit target selector
void DrawUsdPrimEditTarget(UsdPrim &prim) {
    using Query = pxr::UsdPrimCompositionQuery;
    auto filter = UsdPrimCompositionQuery::Filter();
    pxr::UsdPrimCompositionQuery arc(prim, filter);
    auto compositionArcs = arc.GetCompositionArcs();
    if (ImGui::MenuItem("Session layer")) {
        ExecuteAfterDraw(&UsdStage::SetEditTarget, prim.GetStage(), UsdEditTarget(prim.GetStage()->GetSessionLayer()));
    }
    if (ImGui::MenuItem("Root layer")) {
        ExecuteAfterDraw(&UsdStage::SetEditTarget, prim.GetStage(), UsdEditTarget(prim.GetStage()->GetRootLayer()));
    }
    for (auto a : compositionArcs) {
        // NOTE: we can use GetIntroducingLayer() and GetIntroducingPrimPath() to add more information
        if (a.GetTargetNode()) {
            std::string arcName = a.GetTargetNode().GetLayerStack()->GetIdentifier().rootLayer->GetDisplayName() + " " +
                                  a.GetTargetNode().GetPath().GetString();
            if (ImGui::MenuItem(arcName.c_str())) {
                ExecuteAfterDraw(&UsdStage::SetEditTarget, prim.GetStage(),
                                 UsdEditTarget(a.GetTargetNode().GetLayerStack()->GetIdentifier().rootLayer, a.GetTargetNode()));
            }
        }
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

void DrawUsdPrimHeader(UsdPrim &prim) {
    auto editTarget = prim.GetStage()->GetEditTarget();
    const SdfPath targetPath = editTarget.MapToSpecPath(prim.GetPath());

    if (ImGui::BeginTable("##DrawPropertyEditorHeader", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Field");
        ImGui::TableSetupColumn("Value");
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowHeight);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Stage");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", prim.GetStage()->GetRootLayer()->GetIdentifier().c_str());

        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowHeight);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Path");
        ImGui::TableSetColumnIndex(2);
        {
            ScopedStyleColor pathColor(ImGuiCol_Text, GetPrimColor(prim));
            ImGui::Text("%s", prim.GetPrimPath().GetString().c_str());
        }
        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowHeight);
        ImGui::TableSetColumnIndex(0);
        DrawPropertyMiniButton(ICON_FA_PEN);
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

        ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowHeight);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Type");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", prim.GetTypeName().GetString().c_str());

        ImGui::EndTable();
    }
}

void DrawUsdPrimProperties(UsdPrim &prim, UsdTimeCode currentTime) {

    DrawPropertyEditorMenuBar(prim, 0);

    if (prim) {
        auto headerSize = ImGui::GetWindowSize();
        headerSize.y = TableRowHeight*4; // 4 fields in the header
        ImGui::BeginChild("##Header", headerSize);
        DrawUsdPrimHeader(prim);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::BeginChild("##Body");
        if (DrawAssetInfo(prim)) {
            ImGui::Separator();
        }

        // Draw variant sets
        if (DrawVariantSetsCombos(prim)) {
            ImGui::Separator();
        }

        // Transforms
        if (DrawXformsCommon(prim, currentTime)) {
            ImGui::Separator();
        }

        if (ImGui::BeginTable("##DrawPropertyEditorTable", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
            ImGui::TableSetupColumn("Property name");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();

            int miniButtonId = 0;
            const auto &editTarget = prim.GetStage()->GetEditTarget();

            // Draw attributes
            for (auto &attribute : prim.GetAttributes()) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowHeight);
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(miniButtonId++);
                DrawPropertyMiniButton(attribute, editTarget, currentTime);
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                DrawAttributeDisplayName(attribute);

                ImGui::TableSetColumnIndex(2);
                ImGui::PushItemWidth(-FLT_MIN); // Right align and get rid of widget label
                DrawAttributeValueAtTime(attribute, currentTime);
                ImGui::PopItemWidth();
                // TODO: in the hint ???
                // DrawAttributeTypeInfo(attribute);
            }

            // Draw relations
            for (auto &relationship : prim.GetRelationships()) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(miniButtonId++);
                DrawPropertyMiniButton(relationship, editTarget, currentTime);
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                DrawUsdRelationshipDisplayName(relationship);

                ImGui::TableSetColumnIndex(2);
                DrawUsdRelationshipList(relationship);
            }

            ImGui::EndTable();
        }
        ImGui::EndChild();
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
        GfVec3f translationf(translation[0], translation[1], translation[2]);
        if (ImGui::BeginTable("##DrawXformsCommon", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
            ImGui::TableSetupColumn("Transform");
            ImGui::TableSetupColumn("Value");

            ImGui::TableHeadersRow();
            // Translate
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("translation");

            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::InputFloat3("Translation", translationf.data(), DecimalPrecision);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                translation[0] = translationf[0]; // TODO: InputDouble3 instead, we don't want to loose values
                translation[1] = translationf[1];
                translation[2] = translationf[2];
                ExecuteAfterDraw(&UsdGeomXformCommonAPI::SetTranslate, xformAPI, translation, currentTime);
            }
            ImGui::PopItemWidth();
            // Rotation
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("rotation");

            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::InputFloat3("Rotation", rotation.data(), DecimalPrecision);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ExecuteAfterDraw(&UsdGeomXformCommonAPI::SetRotate, xformAPI, rotation, rotOrder, currentTime);
            }
            ImGui::PopItemWidth();
            // Scale
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("scale");

            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::InputFloat3("Scale", scale.data(), DecimalPrecision);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ExecuteAfterDraw(&UsdGeomXformCommonAPI::SetScale, xformAPI, scale, currentTime);
            }
            ImGui::PopItemWidth();
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("pivot");

            ImGui::TableSetColumnIndex(2);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::InputFloat3("Pivot", pivot.data(), DecimalPrecision);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ExecuteAfterDraw(&UsdGeomXformCommonAPI::SetPivot, xformAPI, pivot, currentTime);
            }
            ImGui::PopItemWidth();
            // TODO rotation order
            ImGui::EndTable();
        }
        return true;
    }
    return false;
}
