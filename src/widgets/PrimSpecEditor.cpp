
#include <iostream>
#include <array>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/propertySpec.h>

#include "ImGuiHelpers.h"
#include "PrimSpecEditor.h"
#include "CompositionEditor.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "Commands.h"
#include "Constants.h"
#include "ValueEditor.h"
#include "LayerEditor.h"
#include "VariantEditor.h"
#include "ProxyHelpers.h"



void DrawPrimSpecifierCombo(SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags) {

    const SdfSpecifier current = primSpec->GetSpecifier();
    SdfSpecifier selected = current;
    const std::string specifierName = TfEnum::GetDisplayName(current);
    if (ImGui::BeginCombo("Specifier", specifierName.c_str(), comboFlags)) {
        for (int n = SdfSpecifierDef; n < SdfNumSpecifiers; n++) {
            const SdfSpecifier displayed = static_cast<SdfSpecifier>(n);
            const bool isSelected = (current == displayed);
            if (ImGui::Selectable(TfEnum::GetDisplayName(displayed).c_str(), isSelected)) {
                selected = displayed;
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
        }

        if (selected != current) {
            ExecuteAfterDraw(&SdfPrimSpec::SetSpecifier, primSpec, selected);
        }

        ImGui::EndCombo();
    }
}


// TODO Share code as we want to share the style of the button, but not necessarily the behaviour
// DrawMiniButton ?? in a specific file ? OptionButton ??? OptionMenuButton ??
static void DrawPropertyMiniButton(const char *btnStr, int rowId, const ImVec4 &btnColor = ImVec4({0.0, 0.7, 0.0, 1.0})) {
    ImGui::PushID(rowId);
    ImGui::PushStyleColor(ImGuiCol_Text, btnColor);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::SmallButton(btnStr);
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void DrawPrimInstanceableActionButton(SdfPrimSpecHandle &primSpec, int buttonId) {

    DrawPropertyMiniButton("(m)", buttonId);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        if (primSpec->HasInstanceable()) {
            if (ImGui::Button("Reset to default")) {
                ExecuteAfterDraw(&SdfPrimSpec::ClearInstanceable, primSpec);
            }
        }
        ImGui::EndPopup();
    }
}

void DrawPrimInstanceable(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isInstanceable = primSpec->GetInstanceable();
    if (ImGui::Checkbox("Instanceable", &isInstanceable)) {
        ExecuteAfterDraw(&SdfPrimSpec::SetInstanceable, primSpec, isInstanceable);
    }
}

void DrawPrimHidden(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isHidden = primSpec->GetHidden();
    if (ImGui::Checkbox("Hidden", &isHidden)) {
        ExecuteAfterDraw(&SdfPrimSpec::SetHidden, primSpec, isHidden);
    }
}

void DrawPrimActive(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isActive = primSpec->GetActive();
    if (ImGui::Checkbox("Active", &isActive)) {
        ExecuteAfterDraw(&SdfPrimSpec::SetActive, primSpec, isActive);
    }
    // TODO Move the clear button into a menu item :Reset to default
    if (primSpec->HasActive()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            ExecuteAfterDraw(&SdfPrimSpec::ClearActive, primSpec);
        }
    }
}

void DrawPrimName(SdfPrimSpecHandle &primSpec) {
    auto nameBuffer = primSpec->GetName();
    ImGui::InputText("Prim Name", &nameBuffer);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto primName = std::string(const_cast<char *>(nameBuffer.data()));
        if (primSpec->CanSetName(primName, nullptr)) {
            ExecuteAfterDraw(&SdfPrimSpec::SetName, primSpec, primName, true);
        }
    }
}

void DrawPrimKind(SdfPrimSpecHandle &primSpec) {
    auto primKind = primSpec->GetKind();
    if (ImGui::BeginCombo("Kind", primKind.GetString().c_str())) {
        for (auto kind: KindRegistry::GetAllKinds()){
            bool isSelected = primKind == kind;
            if (ImGui::Selectable(kind.GetString().c_str(), isSelected)) {
                ExecuteAfterDraw(&SdfPrimSpec::SetKind, primSpec, kind);
            }
        }
        ImGui::EndCombo();
    }
    if (primSpec->HasKind()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            ExecuteAfterDraw(&SdfPrimSpec::ClearKind, primSpec);
        }
    }
}

/// Convert prim class tokens to and from char *
/// The chars are stored in DrawPrimType
static inline const char *ClassCharFromToken(const TfToken &classToken) {
    return classToken == SdfTokens->AnyTypeToken ? "" : classToken.GetString().c_str();
}

static inline TfToken ClassTokenFromChar(const char *classChar) {
    return strcmp(classChar, "") == 0 ? SdfTokens->AnyTypeToken : TfToken(classChar);
}

/// Draw a prim type name combo
void DrawPrimType(SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags) {

    /// TODO reset to none as well
    /// TODO: look at: https://github.com/ocornut/imgui/issues/282
    // TODO: get all types, ATM the function GetAllTypes is not exposed by the api, missing SDF_API
    // auto allTypes = primSpec->GetSchema().GetAllTypes();
    // auto allTypes = SdfSchema::GetInstance().GetAllTypes();
    // They are also registered in "registry.usda"
    const char *allTypes[] = {"",       "Scope",     "Xform",          "Cube", "Sphere",     "Cylinder",     "Capsule",
                              "Cone",   "Camera",    "PointInstancer", "Mesh", "GeomSubset", "DistantLight", "Material",
                              "Shader", "BlendShape"};

    const char *currentItem = ClassCharFromToken(primSpec->GetTypeName());
    if (ImGui::BeginCombo("Prim Type", currentItem, comboFlags)) {
        for (int n = 0; n < IM_ARRAYSIZE(allTypes); n++) {
            const bool isSelected = strcmp(currentItem, allTypes[n]) == 0;
            if (ImGui::Selectable(allTypes[n], isSelected)) {
                currentItem = allTypes[n];
            }
        }

        if (primSpec->GetTypeName() != ClassTokenFromChar(currentItem)) {
            ExecuteAfterDraw(&SdfPrimSpec::SetTypeName, primSpec, ClassTokenFromChar(currentItem));
        }
        ImGui::EndCombo();
    }
}



void DrawPrimSpecAttributes(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;

    int deleteButtonCounter = 0;
    const auto &attributes = primSpec->GetProperties();
    if (attributes.empty()) return;
    static ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##DrawPrimSpecAttributes", 4, tableFlags)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Attribute");
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Value");

        ImGui::TableHeadersRow();

        TF_FOR_ALL(attribute, attributes) {
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;

            ImGui::TableNextRow();

            // MiniButton
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(deleteButtonCounter++);
            if (ImGui::Button(ICON_FA_TRASH)) {
                ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath((*attribute)->GetPath()));
            }
            ImGui::PopID();

            // Name of the parameter
            ImGui::TableSetColumnIndex(1);
            SdfTimeSampleMap timeSamples = (*attribute)->GetTimeSampleMap();
            if (timeSamples.empty())
                nodeFlags |= ImGuiTreeNodeFlags_Leaf;
            bool unfolded = ImGui::TreeNodeEx((*attribute)->GetName().c_str(), nodeFlags, "%s", (*attribute)->GetName().c_str());
            if ((*attribute)->HasDefaultValue()) { // If closed show the default value
                ImGui::TableSetColumnIndex(2);
                const std::string defaultValueLabel = "Default";
                ImGui::Text("%s", defaultValueLabel.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::PushItemWidth(-FLT_MIN);
                VtValue modified = DrawVtValue(defaultValueLabel, (*attribute)->GetDefaultValue());
                 if (modified != VtValue()) {
                    ExecuteAfterDraw(&SdfPropertySpec::SetDefaultValue, *attribute, modified);
                }
            }
            if (unfolded) {
                TF_FOR_ALL(sample, timeSamples) { // Samples
                    ImGui::TableNextRow();
                    // Mini button
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(deleteButtonCounter++);
                    if (ImGui::Button(ICON_FA_TRASH)) {
                        ExecuteAfterDraw(&SdfLayer::EraseTimeSample, primSpec->GetLayer(), (*attribute)->GetPath(),
                                         sample->first);
                    }
                    ImGui::PopID();
                    // Time: TODO edit time ?
                    ImGui::TableSetColumnIndex(2);
                    std::string sampleValueLabel = std::to_string(sample->first);
                    ImGui::Text("%s", sampleValueLabel.c_str());
                    // Value
                    ImGui::TableSetColumnIndex(3);
                    ImGui::PushItemWidth(-FLT_MIN);
                    VtValue modified = DrawVtValue(sampleValueLabel, sample->second);
                    if (modified != VtValue()) {
                        ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, primSpec->GetLayer(), (*attribute)->GetPath(),
                                         sample->first, modified);
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }
}

/// Using templates to factor the code in DrawPrimSpecMetadata
template <typename MetadataField> const char *NameForMetadataField();
template <typename MetadataField> void DrawMetadataFieldValue(SdfPrimSpecHandle &primSpec);
template <typename MetadataField> void DrawMetadataFieldButton(SdfPrimSpecHandle& primSpec, int rowId) { DrawPropertyMiniButton("(m)", rowId); }

/// Draw one row of the metadata field
template <typename MetadataField> void DrawMetadataRow(SdfPrimSpecHandle &primSpec, int rowId) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawMetadataFieldButton<MetadataField>(primSpec, rowId);
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", NameForMetadataField<MetadataField>());
    ImGui::TableSetColumnIndex(2);
    ImGui::PushItemWidth(-FLT_MIN);
    DrawMetadataFieldValue<MetadataField>(primSpec);
}

// Definitions for Metadata field
struct PrimName {};
template <> const char *NameForMetadataField<PrimName>() { return "Name"; }
template <> void DrawMetadataFieldValue<PrimName>(SdfPrimSpecHandle &primSpec) { DrawPrimName(primSpec); }

struct Specifier {};
template <> const char *NameForMetadataField<Specifier>() { return "Specifier"; }
template <> void DrawMetadataFieldValue<Specifier>(SdfPrimSpecHandle &primSpec) { DrawPrimSpecifierCombo(primSpec); }

struct PrimType {};
template <> const char *NameForMetadataField<PrimType>() { return "Type"; }
template <> void DrawMetadataFieldValue<PrimType>(SdfPrimSpecHandle &primSpec) { DrawPrimType(primSpec); }

struct PrimKind {};
template <> const char *NameForMetadataField<PrimKind>() { return "Kind"; }
template <> void DrawMetadataFieldValue<PrimKind>(SdfPrimSpecHandle &primSpec) { DrawPrimKind(primSpec); }

struct PrimActive {};
template <> const char *NameForMetadataField<PrimActive>() { return "Active"; }
template <> void DrawMetadataFieldValue<PrimActive>(SdfPrimSpecHandle &primSpec) { DrawPrimActive(primSpec); }

struct Instanceable {};
template <> const char *NameForMetadataField<Instanceable>() { return "Instanceable"; }
template <> void DrawMetadataFieldValue<Instanceable>(SdfPrimSpecHandle &primSpec) { DrawPrimInstanceable(primSpec); }

struct PrimHidden {};
template <> const char *NameForMetadataField<PrimHidden>() { return "Hidden"; }
template <> void DrawMetadataFieldValue<PrimHidden>(SdfPrimSpecHandle &primSpec) { DrawPrimHidden(primSpec); }



void DrawPrimSpecMetadata(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->GetPath().IsPrimVariantSelectionPath()) {

        int rowId = 0;

        if (ImGui::BeginTable("##DrawPrimSpecMetadata", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            
            TableSetupColumns("", "Metadata", "Value");
            ImGui::TableHeadersRow();

            DrawMetadataRow<Specifier>(primSpec, rowId++);
            DrawMetadataRow<PrimType>(primSpec, rowId++);
            DrawMetadataRow<PrimName>(primSpec, rowId++);
            DrawMetadataRow<PrimKind>(primSpec, rowId++);
            DrawMetadataRow<PrimActive>(primSpec, rowId++);
            DrawMetadataRow<Instanceable>(primSpec, rowId++);
            DrawMetadataRow<PrimHidden>(primSpec, rowId++);

            ImGui::EndTable();

            ImGui::Separator();
        }
    }
}

void DrawPrimSpecEditor(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    ImGui::Text("%s", primSpec->GetLayer()->GetDisplayName().c_str());
    ImGui::Text("%s", primSpec->GetPath().GetString().c_str());

    DrawPrimSpecMetadata(primSpec);
    DrawPrimCompositions(primSpec);
    DrawPrimVariants(primSpec);
    DrawPrimSpecAttributes(primSpec); // properties ???
}
