#include <iostream>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/pcp/node.h>
#include "Gui.h"
#include "PropertyEditor.h"
#include "ValueEditor.h"
#include "Constants.h"
#include "Commands.h"

PXR_NAMESPACE_USING_DIRECTIVE

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
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(str).x -
                         ImGui::GetScrollX() - 2 * ImGui::GetStyle().ItemSpacing.x);
}

void DrawAttributeDisplayName(const UsdAttribute &attribute) {
    const std::string displayName = GetDisplayName(attribute);
    ImGui::Text("%s", displayName.c_str());
}

void DrawAttributeValueAtTime(UsdAttribute &attribute, UsdTimeCode currentTime) {
    std::string attributeLabel = GetDisplayName(attribute);
    VtValue value;
    if (attribute.Get(&value, currentTime)) {
        VtValue modified = DrawAttributeValue(attributeLabel, attribute, value);
        if (!modified.IsEmpty()) {
            ExecuteAfterDraw<AttributeSet>(attribute, modified,
                                           attribute.GetNumTimeSamples() ? currentTime : UsdTimeCode::Default());
        }
    } else {
        // No values, what do we display ??
        // TODO: we should always display the connections
        ImVec4 attributeNameColor = attribute.IsAuthored() ? ImVec4(AttributeAuthoredColor) : ImVec4(AttributeUnauthoredColor);
        if (attribute.HasAuthoredConnections()) {
            SdfPathVector sources;
            attribute.GetConnections(&sources);
            for (auto &connection : sources) {
                // TODO: edit connection
                ImGui::TextColored(ImVec4(AttributeConnectionColor), ICON_FA_LINK " %s", connection.GetString().c_str());
            }
        } else {
            ImGui::TextColored(ImVec4({0.5, 0.5, 0.5, 0.5}), "no value");
        }
    }
}

void DrawUsdRelationshipDisplayName(const UsdRelationship& relationship) {
    std::string relationshipName = GetDisplayName(relationship);
    ImVec4 attributeNameColor = relationship.IsAuthored() ? ImVec4(AttributeAuthoredColor) : ImVec4(AttributeUnauthoredColor);
    ImGui::TextColored(ImVec4(attributeNameColor), "%s", relationshipName.c_str());
}

void DrawUsdRelationshipList(const UsdRelationship &relationship) {
    SdfPathVector targets;
    //relationship.GetForwardedTargets(&targets);
    //for (const auto &path : targets) {
    //    ImGui::TextColored(ImVec4(AttributeRelationshipColor), "%s", path.GetString().c_str());
    //}
    relationship.GetTargets(&targets);
    if (!targets.empty()) {
        if (ImGui::BeginListBox("##Relationship", ImVec2(-FLT_MIN, targets.size() * 25))) {
            for (const auto& path : targets) {
                if (ImGui::Button(ICON_FA_TRASH)) {
                    ExecuteAfterDraw(&UsdRelationship::RemoveTarget, relationship, path);
                } // Trash
                ImGui::SameLine();
                std::string buffer = path.GetString();
                ImGui::InputText("##EditRelation", &buffer);
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
        if (ImGui::MenuItem(ICON_FA_EJECT " Clear")) {
            ExecuteAfterDraw(&UsdAttribute::Clear, attribute);
        }
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
static void DrawPropertyMiniButton(const char *btnStr, const ImVec4 &btnColor = ImVec4(MiniButtonUnauthoredColor)) {
    ImGui::PushStyleColor(ImGuiCol_Text, btnColor);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(TransparentColor));
    ImGui::SmallButton(btnStr);
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
}

// TODO: relationship
template <typename UsdPropertyT>
void DrawMenuCreateValue(UsdPropertyT& property) {};

template<>
void DrawMenuCreateValue(UsdAttribute& attribute) {
    if (!attribute.HasValue()) {
        if (ImGui::MenuItem(ICON_FA_DONATE " Create value")) {
            ExecuteAfterDraw<AttributeCreateDefaultValue>(attribute);
        }
    }
}



// Property mini button, should work with UsdProperty, UsdAttribute and UsdRelationShip
template <typename UsdPropertyT>
void DrawPropertyMiniButton(UsdPropertyT &property, const UsdEditTarget &editTarget, UsdTimeCode currentTime) {
    ImVec4 propertyColor = property.IsAuthoredAt(editTarget) ? ImVec4(MiniButtonAuthoredColor) : ImVec4(MiniButtonUnauthoredColor);
    DrawPropertyMiniButton(SmallButtonLabel<UsdPropertyT>(), propertyColor);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        DrawMenuSetKey(property, currentTime);
        DrawMenuCreateValue(property);
        DrawMenuClearAuthoredValues(property);
        DrawMenuRemoveProperty(property);
        if (ImGui::MenuItem(ICON_FA_COPY " Copy attribute path")) {
            ImGui::SetClipboardText(property.GetPath().GetString().c_str());
        }
        ImGui::EndPopup();
    }
}

bool DrawVariantSetsCombos(UsdPrim &prim) {
    int buttonID = 0;
    if (!prim.HasVariantSets()) return false;
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
                variantSet.HasAuthoredVariantSelection() ? ImVec4(MiniButtonAuthoredColor) : ImVec4(MiniButtonUnauthoredColor);
            ImGui::PushID(buttonID++);
            DrawPropertyMiniButton("(v)", variantColor);
            ImGui::PopID();
            if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft) ){
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
        if (prim && ImGui::BeginMenu(ICON_FA_PLUS)) {

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

void DrawUsdPrimProperties(UsdPrim &prim, UsdTimeCode currentTime) {

    DrawPropertyEditorMenuBar(prim, 0);

    if (prim) {
        auto editTarget = prim.GetStage()->GetEditTarget();
        const SdfPath targetPath = editTarget.MapToSpecPath(prim.GetPath());
        ImGui::Text("%s %s", prim.GetTypeName().GetString().c_str(), prim.GetPrimPath().GetString().c_str());
        ImGui::Text(ICON_FA_PEN " %s %s", editTarget.GetLayer()->GetDisplayName().c_str(), targetPath.GetString().c_str());

        // TODO: Edit in variant context, under a variant
        // This is a test.
        // Notes from the USD forum:
        // The new UsdPrimCompositionQuery API, which should land in the dev branch soon, will allow you to find any reference,
        // and give you exactly what you need to construct an UsdEditTarget to edit the reference, no matter where it was
        // authored. You and the user still need to decide and agree whether you’re only allowing “root layerStack” i.e.
        // non-destructive edits, or whether all referenced layers are game for editing. The Query API can restrict its search to
        // the root layerStack, if you desire. As described at the top of UsdPrimCompositionQueryArc, once you find the inherits
        // (or specializes) arc that "introduces" the class, use that Arc's GetTargetNode() as the PcpNodeRef for a UsdEditTarget,
        // and rummage through its LayerStack's layers to see the layers in which you can edit.
        //UsdPrimCompositionQuery arc(prim);
        //auto compositionArcs = arc.GetCompositionArcs();
        //if (ImGui::BeginListBox("arcs")) {
        //    for (auto a : compositionArcs) {
        //        if (a.GetArcType() == PcpArcType::PcpArcTypeVariant) {
        //            a.GetIntroducingLayer()->GetDisplayName();
        //            std::string arcName =
        //                a.GetIntroducingLayer()->GetDisplayName() + " " + a.GetIntroducingPrimPath().GetString();
        //            if (ImGui::Button(arcName.c_str())) {
        //                ExecuteAfterDraw(&UsdStage::SetEditTarget, prim.GetStage(),
        //                                 UsdEditTarget(editTarget.GetLayer(), a.GetTargetNode()));
        //            }
        //        }
        //    }
        //    if (ImGui::Button("Reset")) {
        //        ExecuteAfterDraw(&UsdStage::SetEditTarget, prim.GetStage(), UsdEditTarget(editTarget.GetLayer()));
        //    }
        //    ImGui::EndListBox();
        //}
        ImGui::Separator();
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

        if (ImGui::BeginTable("##DrawPropertyEditorTable", 3, ImGuiTableFlags_SizingFixedFit|ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
            ImGui::TableSetupColumn("Property name");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();

            int miniButtonId = 0;

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

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawPropertyMiniButton("(x)");

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("pivot");

            ImGui::TableSetColumnIndex(2);
            ImGui::InputFloat3("Pivot", pivot.data(), DecimalPrecision);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ExecuteAfterDraw(&UsdGeomXformCommonAPI::SetPivot, xformAPI, pivot, currentTime);
            }
            // TODO rotation order
            ImGui::EndTable();
        }
        return true;
    }
    return false;
}
