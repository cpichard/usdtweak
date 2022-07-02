
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
#include "SdfPrimEditor.h"
#include "CompositionEditor.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "Commands.h"
#include "Constants.h"
#include "VtValueEditor.h"
#include "SdfLayerEditor.h"
#include "VariantEditor.h"
#include "ProxyHelpers.h"
#include "FieldValueTable.h"

//// NOTES: Sdf API: Removing a variantSet and cleaning it from the list editing
//// -> https://groups.google.com/g/usd-interest/c/OeqtGl_1H-M/m/xjCx3dT9EgAJ

struct CreateAttributeDialog : public ModalDialog {
    CreateAttributeDialog(const SdfPrimSpecHandle &sdfPrim) : _sdfPrim(sdfPrim){};
    ~CreateAttributeDialog() override {}

    void Draw() override {
        ImGui::InputText("Name", &_attributeName);
        if (ImGui::BeginCombo("Type", _typeName.GetAsToken().GetString().c_str())) {
            for (int i = 0; i < GetAllValueTypeNames().size(); i++) {
                if (ImGui::Selectable(GetAllValueTypeNames()[i].GetAsToken().GetString().c_str(), false)) {
                    _typeName = GetAllValueTypeNames()[i];
                }
            }
            ImGui::EndCombo();
        }
        bool varying = _variability == SdfVariabilityVarying;
        if (ImGui::Checkbox("Varying", &varying)) {
            _variability = _variability == SdfVariabilityVarying ? SdfVariabilityUniform : SdfVariabilityVarying;
        }
        ImGui::Checkbox("Custom", &_custom);
        DrawOkCancelModal(
            [&]() { ExecuteAfterDraw<PrimCreateAttribute>(_sdfPrim, _attributeName, _typeName, _variability, _custom); });
    }
    const char *DialogId() const override { return "Create attribute"; }

    const SdfPrimSpecHandle &_sdfPrim;
    std::string _attributeName;
    SdfVariability _variability = SdfVariabilityVarying;
    SdfValueTypeName _typeName = SdfValueTypeNames->Bool;
    bool _custom = true;
};

struct CreateRelationDialog : public ModalDialog {
    CreateRelationDialog(const SdfPrimSpecHandle &sdfPrim) : _sdfPrim(sdfPrim){};
    ~CreateRelationDialog() override {}

    void Draw() override {
        ImGui::InputText("Relationship name", &_relationName);
        ImGui::InputText("Target path", &_targetPath);
        if (ImGui::BeginCombo("Edit list", GetListEditorOperationName(_operation))) {
            for (int i = 0; i < GetListEditorOperationSize(); ++i) {
                if (ImGui::Selectable(GetListEditorOperationName(i), false)) {
                    _operation = i;
                }
            }
            ImGui::EndCombo();
        }

        bool varying = _variability == SdfVariabilityVarying;
        if (ImGui::Checkbox("Varying", &varying)) {
            _variability = _variability == SdfVariabilityVarying ? SdfVariabilityUniform : SdfVariabilityVarying;
        }
        ImGui::Checkbox("Custom", &_custom);

        DrawOkCancelModal([=]() {
            ExecuteAfterDraw<PrimCreateRelationship>(_sdfPrim, _relationName, _variability, _custom, _operation, _targetPath);
        });
    }
    const char *DialogId() const override { return "Create relationship"; }

    const SdfPrimSpecHandle &_sdfPrim;
    std::string _relationName;
    std::string _targetPath;
    int _operation = 0;
    SdfVariability _variability = SdfVariabilityVarying;
    bool _custom = true;
};

struct CreateVariantModalDialog : public ModalDialog {

    CreateVariantModalDialog(const SdfPrimSpecHandle &primSpec) : _primSpec(primSpec){};
    ~CreateVariantModalDialog() override {}

    void Draw() override {
        if (!_primSpec) {
            CloseModal();
            return;
        }
        bool isVariant = _primSpec->GetPath().IsPrimVariantSelectionPath();
        ImGui::InputText("VariantSet name", &_variantSet);
        ImGui::InputText("Variant name", &_variant);
        if (isVariant) {
            ImGui::Checkbox("Add to variant edit list", &_addToEditList);
        }
        //
        if (ImGui::Button("Add")) {
            // TODO The call might not be safe as _primSpec is copied, so create an actual command instead
            std::function<void()> func = [=]() {
                SdfCreateVariantInLayer(_primSpec->GetLayer(), _primSpec->GetPath(), _variantSet, _variant);
                if (isVariant && _addToEditList) {
                    auto ownerPath = _primSpec->GetPath().StripAllVariantSelections();
                    // This won't work on doubly nested variants,
                    // when there is a Prim between the variants
                    auto ownerPrim = _primSpec->GetPrimAtPath(ownerPath);
                    if (ownerPrim) {
                        auto nameList = ownerPrim->GetVariantSetNameList();
                        if (!nameList.ContainsItemEdit(_variantSet)) {
                            ownerPrim->GetVariantSetNameList().Add(_variantSet);
                        }
                    }
                }
            };
            ExecuteAfterDraw<UsdFunctionCall>(_primSpec->GetLayer(), func);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            CloseModal();
        }
    }

    const char *DialogId() const override { return "Add variant"; }
    SdfPrimSpecHandle _primSpec;
    std::string _variantSet;
    std::string _variant;
    bool _addToEditList = false;
};


void DrawPrimSpecifier(const SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags) {

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

void DrawPrimInstanceableActionButton(const SdfPrimSpecHandle &primSpec, int buttonId) {
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

void DrawPrimInstanceable(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isInstanceable = primSpec->GetInstanceable();
    if (ImGui::Checkbox("Instanceable", &isInstanceable)) {
        ExecuteAfterDraw(&SdfPrimSpec::SetInstanceable, primSpec, isInstanceable);
    }
}

void DrawPrimHidden(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isHidden = primSpec->GetHidden();
    if (ImGui::Checkbox("Hidden", &isHidden)) {
        ExecuteAfterDraw(&SdfPrimSpec::SetHidden, primSpec, isHidden);
    }
}

void DrawPrimActive(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isActive = primSpec->GetActive();
    if (ImGui::Checkbox("Active", &isActive)) {
        // TODO: use CTRL click to clear the checkbox
        ExecuteAfterDraw(&SdfPrimSpec::SetActive, primSpec, isActive);
    }
}

void DrawPrimName(const SdfPrimSpecHandle &primSpec) {
    auto nameBuffer = primSpec->GetName();
    ImGui::InputText("Prim Name", &nameBuffer);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto primName = std::string(const_cast<char *>(nameBuffer.data()));
        if (primSpec->CanSetName(primName, nullptr)) {
            ExecuteAfterDraw(&SdfPrimSpec::SetName, primSpec, primName, true);
        }
    }
}

void DrawPrimKind(const SdfPrimSpecHandle &primSpec) {
    auto primKind = primSpec->GetKind();
    if (ImGui::BeginCombo("Kind", primKind.GetString().c_str())) {
        for (auto kind : KindRegistry::GetAllKinds()) {
            bool isSelected = primKind == kind;
            if (ImGui::Selectable(kind.GetString().c_str(), isSelected)) {
                ExecuteAfterDraw(&SdfPrimSpec::SetKind, primSpec, kind);
            }
        }
        ImGui::EndCombo();
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
void DrawPrimType(const SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags) {
    const char *currentItem = ClassCharFromToken(primSpec->GetTypeName());
    const auto &allSpecTypes = GetAllSpecTypeNames();
    static int selected = 0;
    
    if (ComboWithFilter("Prim Type", currentItem, allSpecTypes, &selected, comboFlags)) {
        const auto newSelection = allSpecTypes[selected].c_str();
        if (primSpec->GetTypeName() != ClassTokenFromChar(newSelection)) {
            ExecuteAfterDraw(&SdfPrimSpec::SetTypeName, primSpec, ClassTokenFromChar(newSelection));
        }
    }
}
template<typename T>
static inline void DrawArrayEditorButton(T attribute) {
    if ((*attribute)->GetDefaultValue().IsArrayValued()) {
        if (ImGui::Button(ICON_FA_LIST)) {
            ExecuteAfterDraw<EditorSelectAttributePath>((*attribute)->GetPath());
        }
        ImGui::SameLine();
    }
}

void DrawPrimSpecAttributes(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;

    int deleteButtonCounter = 0;
    const auto &attributes = primSpec->GetAttributes();
    if (attributes.empty())
        return;
    static ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##DrawPrimSpecAttributes", 4, tableFlags)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

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
                DrawArrayEditorButton(attribute);
                ImGui::PushItemWidth(-FLT_MIN);
                VtValue modified = DrawVtValue(defaultValueLabel, (*attribute)->GetDefaultValue());
                if (modified != VtValue()) {
                    ExecuteAfterDraw(&SdfPropertySpec::SetDefaultValue, *attribute, modified);
                }
                ImGui::PopItemWidth();
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
                    DrawArrayEditorButton(attribute);
                    ImGui::PushItemWidth(-FLT_MIN);
                    VtValue modified = DrawVtValue(sampleValueLabel, sample->second);
                    if (modified != VtValue()) {
                        ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, primSpec->GetLayer(), (*attribute)->GetPath(),
                                         sample->first, modified);
                    }
                    ImGui::PopItemWidth();
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }
}

static void DrawRelashionshipEditListItem(const char *operation, const SdfPath &path, int &id, const SdfPrimSpecHandle &primSpec,
                                   SdfPath &relationPath) {
    std::string newPathStr = path.GetString();
    ImGui::PushID(id++);
    ImGui::Text("%s", operation);
    ImGui::SameLine();
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::InputText("###RelationValue", &newPathStr);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::function<void()> replaceItemEdits = [=]() {
            if (primSpec) {
                const auto &relationship = primSpec->GetRelationshipAtPath(relationPath);
                if (relationship) {
                    relationship->GetTargetPathList().ReplaceItemEdits(path, SdfPath(newPathStr));
                }
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), replaceItemEdits);
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
}


struct RelationField {};
template <>
inline void DrawFieldName<RelationField>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                         const SdfRelationshipSpecHandle &relation) {
    ImGui::Text("%s", relation->GetName().c_str());
};
template <>
inline void DrawFieldValue<RelationField>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                          const SdfRelationshipSpecHandle &relation) {
    IterateListEditorItems(relation->GetTargetPathList(), DrawRelashionshipEditListItem, rowId, primSpec, relation->GetPath());
};
template <>
inline void DrawFieldButton<RelationField>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                           const SdfRelationshipSpecHandle &relation) {
    if (ImGui::Button(ICON_FA_COG)) {
        ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath(relation->GetPath()));
    }
};

void DrawPrimSpecRelations(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    const auto &relationships = primSpec->GetRelationships();
    if (relationships.empty())
        return;
    int rowId = 0;
    if (BeginFieldValueTable("##DrawPrimSpecRelations")) {
        SetupFieldValueTableColumns(true, "", "Relations", "");
        auto relations = primSpec->GetRelationships();
        for (const SdfRelationshipSpecHandle &relation : relations) {
            DrawFieldValueTableRow<RelationField>(rowId, primSpec, relation);
        }
        ImGui::EndTable();
    }
}


#define GENERATE_FIELD(ClassName_, FieldName_, DrawFunction_)                                                                    \
    struct ClassName_ {                                                                                                          \
        static constexpr const char *fieldName = FieldName_;                                                                     \
    };                                                                                                                           \
    template <> inline void DrawFieldValue<ClassName_>(const int rowId, const SdfPrimSpecHandle &primSpec) {                     \
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);                                                               \
        DrawFunction_(primSpec);                                                                                                 \
    }

#define GENERATE_FIELD_WITH_BUTTON(ClassName_, Token_, FieldName_, DrawFunction_)                                                \
    GENERATE_FIELD(ClassName_, FieldName_, DrawFunction_);                                                                       \
    template <> inline bool HasEdits<ClassName_>(const SdfPrimSpecHandle &prim) { return prim->HasField(Token_); }               \
    template <> inline void DrawFieldButton<ClassName_>(const int rowId, const SdfPrimSpecHandle &primSpec) {                    \
        ImGui::PushID(rowId);                                                                                                    \
        if (ImGui::Button(ICON_FA_TRASH) && HasEdits<ClassName_>(primSpec)) {                                                    \
            ExecuteAfterDraw(&SdfPrimSpec::ClearField, primSpec, Token_);                                                        \
        }                                                                                                                        \
        ImGui::PopID();                                                                                                          \
    }\


GENERATE_FIELD(Specifier, "Specifier", DrawPrimSpecifier);
GENERATE_FIELD(PrimType,  "Type", DrawPrimType);
GENERATE_FIELD(PrimName, "Name", DrawPrimName);
GENERATE_FIELD_WITH_BUTTON(PrimKind, SdfFieldKeys->Kind, "Kind", DrawPrimKind);
GENERATE_FIELD_WITH_BUTTON(PrimActive, SdfFieldKeys->Active, "Active", DrawPrimActive);
GENERATE_FIELD_WITH_BUTTON(PrimInstanceable, SdfFieldKeys->Instanceable, "Instanceable", DrawPrimInstanceable);
GENERATE_FIELD_WITH_BUTTON(PrimHidden, SdfFieldKeys->Hidden, "Hidden", DrawPrimHidden);


void DrawPrimSpecMetadata(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec->GetPath().IsPrimVariantSelectionPath()) {
        int rowId = 0;
        if (BeginFieldValueTable("##DrawPrimSpecMetadata")) {
            SetupFieldValueTableColumns(true, "", "Metadata", "Value");
            DrawFieldValueTableRow<Specifier>(rowId++, primSpec);
            DrawFieldValueTableRow<PrimType>(rowId++, primSpec);
            DrawFieldValueTableRow<PrimName>(rowId++, primSpec);
            DrawFieldValueTableRow<PrimKind>(rowId++, primSpec);
            DrawFieldValueTableRow<PrimActive>(rowId++, primSpec);
            DrawFieldValueTableRow<PrimInstanceable>(rowId++, primSpec);
            DrawFieldValueTableRow<PrimHidden>(rowId++, primSpec);
            EndFieldValueTable();
        }
        ImGui::Separator();
    }
}

static void DrawPrimAssetInfo(const SdfPrimSpecHandle prim) {
    if (!prim)
        return;
    const auto &assetInfo = prim->GetAssetInfo();
    if (!assetInfo.empty()) {
        if (ImGui::BeginTable("##DrawAssetInfo", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            TableSetupColumns("", "Asset Info", "");
            ImGui::TableHeadersRow();
            TF_FOR_ALL(keyValue, assetInfo) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", keyValue->first.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::PushItemWidth(-FLT_MIN);
                VtValue modified = DrawVtValue(keyValue->first.c_str(), keyValue->second);
                if (modified != VtValue()) {
                    ExecuteAfterDraw(&SdfPrimSpec::SetAssetInfo, prim, keyValue->first, modified);
                }
                ImGui::PopItemWidth();
            }
            ImGui::EndTable();
            ImGui::Separator();
        }
    }
}

void DrawPrimCreateCompositionMenu(const SdfPrimSpecHandle &primSpec) {
    if (primSpec) {
        if (ImGui::MenuItem("Reference")) {
            DrawPrimCreateReference(primSpec);
        }
        if (ImGui::MenuItem("Payload")) {
            DrawPrimCreatePayload(primSpec);
        }
        if (ImGui::MenuItem("Inherit")) {
            DrawPrimCreateInherit(primSpec);
        }
        if (ImGui::MenuItem("Specialize")) {
            DrawPrimCreateSpecialize(primSpec);
        }
        if (ImGui::MenuItem("Variant")) {
            DrawModalDialog<CreateVariantModalDialog>(primSpec);
        }
    }
}

void DrawPrimCreatePropertyMenu(const SdfPrimSpecHandle &primSpec) {
    if (primSpec) {
        if (ImGui::MenuItem("Attribute")) {
            DrawModalDialog<CreateAttributeDialog>(primSpec);
        }
        if (ImGui::MenuItem("Relation")) {
            DrawModalDialog<CreateRelationDialog>(primSpec);
        }
    }
}


void DrawSdfPrimEditorMenuBar(const SdfPrimSpecHandle &primSpec) {

    bool enabled = primSpec;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Create", enabled)) {
            DrawPrimCreateCompositionMenu(primSpec);
            ImGui::Separator();
            DrawPrimCreatePropertyMenu(primSpec); // attributes and relation
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}


void DrawSdfPrimEditor(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    auto headerSize = ImGui::GetWindowSize();
    headerSize.y = TableRowDefaultHeight * 3; // 3 fields in the header
    headerSize.x = -FLT_MIN;
    ImGui::BeginChild("##LayerHeader", headerSize);
    DrawSdfLayerIdentity(primSpec->GetLayer(), primSpec->GetPath()); // TODO rename to DrawUsdObjectInfo()
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginChild("##LayerBody");
    DrawPrimSpecMetadata(primSpec);
    DrawPrimAssetInfo(primSpec);
    DrawPrimCompositions(primSpec);
    DrawPrimVariants(primSpec);
    DrawPrimSpecAttributes(primSpec);
    DrawPrimSpecRelations(primSpec);
    ImGui::EndChild();
}
