#include <iostream>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/gprim.h>
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
        return DrawColorValue(label, value);
    }
    return DrawVtValue(label, value);
}

void DrawUsdAttribute(UsdAttribute &attribute, UsdTimeCode currentTime) {
    std::string attributeLabel = attribute.GetNamespace().GetString() +
                                (attribute.GetNamespace() == TfToken() ? std::string() : std::string(":")) +
                                attribute.GetBaseName().GetString();
    VtValue value;
    if (attribute.Get(&value, currentTime)) {
        VtValue modified = DrawAttributeValue(attributeLabel, attribute, value);
        if (!modified.IsEmpty()) {
            ExecuteAfterDraw<AttributeSet>(attribute, modified, currentTime);
        }
        auto attributeTypeName = attribute.GetTypeName();
        auto attributeRoleName = attribute.GetRoleName();
        ImGui::SameLine();
        ImGui::Text("%s(%s)",  attributeRoleName.GetString().c_str(), attributeTypeName.GetAsToken().GetString().c_str());
    } else {
        ImVec4 attributeNameColor = attribute.IsAuthored() ? ImVec4(AttributeAuthoredColor) : ImVec4(AttributeUnauthoredColor);
        ImGui::TextColored(attributeNameColor, "'%s'", attributeLabel.c_str());
        if (attribute.HasAuthoredConnections()) {
            SdfPathVector sources;
            attribute.GetConnections(&sources);
            for (auto &connection : sources) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(AttributeConnectionColor), "-> %s", connection.GetString().c_str());
            }
        } else {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4({0.5, 0.5, 0.5, 0.5}), "no value");
        }
    }
}

void DrawUsdRelationship(UsdRelationship &relationship) {
    std::string relationshipName = relationship.GetNamespace().GetString()
        + (relationship.GetNamespace() == TfToken() ? std::string() : std::string(":"))
        + relationship.GetBaseName().GetString();
    ImVec4 attributeNameColor = relationship.IsAuthored() ? ImVec4(AttributeAuthoredColor) : ImVec4(AttributeUnauthoredColor);
    ImGui::TextColored(ImVec4(attributeNameColor), "'%s'", relationshipName.c_str());
    SdfPathVector targets;
    //relationship.GetTargets(&targets); TODO: what is the difference wiht GetForwardedTargets
    relationship.GetForwardedTargets(&targets);
    if (targets.size()){
        ImGui::SameLine();
        for (const auto &path : targets) {
            ImGui::TextColored(ImVec4(AttributeRelationshipColor), "-> %s", path.GetString().c_str());
        }
    }
}

void DrawUsdPrimProperties(UsdPrim &prim, UsdTimeCode currentTime) {
    if (prim) {
        auto editTarget = prim.GetStage()->GetEditTarget();
        ImGui::Text("%s %s",  prim.GetTypeName().GetString().c_str(),  prim.GetPrimPath().GetString().c_str());
        auto attributes = prim.GetAttributes();
        for (auto &att : attributes) {
            ImVec4 attColor = att.IsAuthoredAt(editTarget) ? ImVec4({ 0.0, 1.0, 0.0, 1.0 }) : ImVec4({ 0.0, 0.7, 0.0, 1.0 });
            ImGui::TextColored(attColor, "(a)");
            ImGui::SameLine();
            DrawUsdAttribute(att, currentTime);
        }

        auto relationships = prim.GetRelationships();
        for (auto &rel : relationships) {
            ImVec4 attColor = rel.IsAuthoredAt(editTarget) ? ImVec4({ 0.0, 1.0, 0.0, 1.0 }) : ImVec4({ 0.0, 0.7, 0.0, 1.0 });
            ImGui::TextColored(attColor, "(r)");
            ImGui::SameLine();
            DrawUsdRelationship(rel);
        }
    }
}
