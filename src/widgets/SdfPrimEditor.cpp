
#include <array>
#include <iostream>

#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usd/typed.h>
#include <pxr/base/plug/registry.h>

#include "Commands.h"
#include "CompositionEditor.h"
#include "Constants.h"
#include "FileBrowser.h"
#include "ImGuiHelpers.h"
#include "ModalDialogs.h"
#include "UsdHelpers.h"
#include "EditListSelector.h"
#include "SdfLayerEditor.h"
#include "SdfPrimEditor.h"
#include "SdfAttributeEditor.h" // timesample editor
#include "Shortcuts.h"
#include "TableLayouts.h"
#include "VariantEditor.h"
#include "VtDictionaryEditor.h"
#include "VtValueEditor.h"

//// NOTES: Sdf API: Removing a variantSet and cleaning it from the list editing
//// -> https://groups.google.com/g/usd-interest/c/OeqtGl_1H-M/m/xjCx3dT9EgAJ

// TODO: by default use the schema type of the prim
// TODO: copy metadata ? interpolation, etc ??
// TODO: write the code for creating relationship from schema
// TODO: when a schema has no attribute, the ok button should be disabled
//       and the name should be empty instead of the widget disappearing

std::vector<std::string> GetPrimSpecPropertyNames(TfToken primTypeName, SdfSpecType filter) {
    std::vector<std::string> filteredPropNames;
    if (auto *primDefinition = UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(primTypeName)) {
        for (const auto &propName : primDefinition->GetPropertyNames()) {
            if (primDefinition->GetSpecType(propName) == filter) {
                filteredPropNames.push_back(propName.GetString());
            }
        }
    }
    return filteredPropNames;
}

struct CreateAttributeDialog : public ModalDialog {
    CreateAttributeDialog(const SdfPrimSpecHandle &sdfPrim) : _sdfPrim(sdfPrim), _schemaTypeNames(GetAllSpecTypeNames()) {
        // https://openusd.org/release/api/class_usd_prim_definition.html
        selectedSchemaTypeName = std::find(_schemaTypeNames.begin(), _schemaTypeNames.end(), _sdfPrim->GetTypeName().GetString());
        FillAttributeNames();
        UpdateWithNewPrimDefinition();
    };
    ~CreateAttributeDialog() override {}

    void UpdateWithNewPrimDefinition() {
        if (selectedSchemaTypeName != _schemaTypeNames.end() && _selectedAttributeName != _allAttrNames.end()) {
            if (auto *primDefinition =
                    UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(TfToken(*selectedSchemaTypeName))) {
                _attributeName = *_selectedAttributeName;
                SdfAttributeSpecHandle attrSpec = primDefinition->GetSchemaAttributeSpec(TfToken(_attributeName));
                _typeName = attrSpec->GetTypeName();
                _variability = attrSpec->GetVariability();
            }
        } else {
            _attributeName = "";
        }
    }

    void FillAttributeNames() {
        if (selectedSchemaTypeName != _schemaTypeNames.end()) {
            _allAttrNames = GetPrimSpecPropertyNames(TfToken(*selectedSchemaTypeName), SdfSpecTypeAttribute);
            _selectedAttributeName = _allAttrNames.begin();
        } else {
            _attributeName = "";
        }
    }

    void Draw() override {
        // Schema
        ImGui::Checkbox("Find attribute in schema", &_useSchema);
        ImGui::BeginDisabled(!_useSchema);
        const char *schemaStr = selectedSchemaTypeName == _schemaTypeNames.end() ? "" : selectedSchemaTypeName->c_str();
        if (ImGui::BeginCombo("From schema", schemaStr)) {
            for (size_t i = 0; i < _schemaTypeNames.size(); i++) {
                if (_schemaTypeNames[i] != "" && ImGui::Selectable(_schemaTypeNames[i].c_str(), false)) {
                    selectedSchemaTypeName = _schemaTypeNames.begin() + i;
                    FillAttributeNames();
                    UpdateWithNewPrimDefinition();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        // Attribute name
        if (!_useSchema) {
            ImGui::InputText("Name", &_attributeName);
        } else {
            if (ImGui::BeginCombo("Name", _attributeName.c_str())) {
                for (size_t i = 0; i < _allAttrNames.size(); i++) {
                    if (ImGui::Selectable(_allAttrNames[i].c_str(), false)) {
                        _selectedAttributeName = _allAttrNames.begin() + i;
                        UpdateWithNewPrimDefinition();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::BeginDisabled(_useSchema);
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
        ImGui::EndDisabled();

        ImGui::Checkbox("Custom attribute", &_custom);
        ImGui::Checkbox("Create default value", &_createDefault);
        DrawOkCancelModal(
            [&]() {
                ExecuteAfterDraw<PrimCreateAttribute>(_sdfPrim, _attributeName, _typeName, _variability, _custom, _createDefault);
            },
            bool(_attributeName == ""));
    }

    const char *DialogId() const override { return "Create attribute"; }

    const SdfPrimSpecHandle &_sdfPrim;
    std::string _attributeName;
    SdfVariability _variability = SdfVariabilityVarying;
    SdfValueTypeName _typeName = SdfValueTypeNames->Bool;
    bool _custom = false;
    bool _createDefault = false;
    bool _useSchema = true;
    std::vector<std::string> _schemaTypeNames;
    std::vector<std::string>::iterator selectedSchemaTypeName = _schemaTypeNames.end();

    std::vector<std::string> _allAttrNames;
    std::vector<std::string>::iterator _selectedAttributeName = _allAttrNames.end();
};

struct CreateRelationDialog : public ModalDialog {
    CreateRelationDialog(const SdfPrimSpecHandle &sdfPrim) : _sdfPrim(sdfPrim), _allSchemaNames(GetAllSpecTypeNames()) {
        _selectedSchemaName = std::find(_allSchemaNames.begin(), _allSchemaNames.end(), _sdfPrim->GetTypeName().GetString());
        FillRelationshipNames();
        UpdateWithNewPrimDefinition();
    };
    ~CreateRelationDialog() override {}

    void FillRelationshipNames() {
        if (_selectedSchemaName != _allSchemaNames.end()) {
            _allRelationshipNames = GetPrimSpecPropertyNames(TfToken(*_selectedSchemaName), SdfSpecTypeRelationship);
            _selectedRelationshipName = _allRelationshipNames.begin();
        } else {
            _relationName = "";
        }
    }

    void UpdateWithNewPrimDefinition() {
        if (_selectedSchemaName != _allSchemaNames.end() && _selectedRelationshipName != _allRelationshipNames.end()) {
            if (auto *primDefinition =
                    UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(TfToken(*_selectedSchemaName))) {
                _relationName = *_selectedRelationshipName;
                SdfRelationshipSpecHandle relSpec = primDefinition->GetSchemaRelationshipSpec(TfToken(_relationName));
                _variability = relSpec->GetVariability();
            }
        } else {
            _relationName = "";
        }
    }

    void Draw() override {
        ImGui::Checkbox("Find relation in schema", &_useSchema);
        ImGui::BeginDisabled(!_useSchema);
        const char *schemaStr = _selectedSchemaName == _allSchemaNames.end() ? "" : _selectedSchemaName->c_str();
        if (ImGui::BeginCombo("From schema", schemaStr)) {
            for (size_t i = 0; i < _allSchemaNames.size(); i++) {
                if (_allSchemaNames[i] != "" && ImGui::Selectable(_allSchemaNames[i].c_str(), false)) {
                    _selectedSchemaName = _allSchemaNames.begin() + i;
                    FillRelationshipNames();
                    UpdateWithNewPrimDefinition();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        if (!_useSchema) {
            ImGui::InputText("Relationship name", &_relationName);
        } else {
            if (ImGui::BeginCombo("Name", _relationName.c_str())) {
                for (size_t i = 0; i < _allRelationshipNames.size(); i++) {
                    if (ImGui::Selectable(_allRelationshipNames[i].c_str(), false)) {
                        _selectedRelationshipName = _allRelationshipNames.begin() + i;
                        UpdateWithNewPrimDefinition();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::InputText("Target path", &_targetPath);
        if (ImGui::BeginCombo("Edit list", GetListEditorOperationName(_operation))) {
            for (int i = 0; i < GetListEditorOperationSize(); ++i) {
                if (ImGui::Selectable(GetListEditorOperationName(i), false)) {
                    _operation = static_cast<SdfListOpType>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(_useSchema);
        bool varying = _variability == SdfVariabilityVarying;
        if (ImGui::Checkbox("Varying", &varying)) {
            _variability = _variability == SdfVariabilityVarying ? SdfVariabilityUniform : SdfVariabilityVarying;
        }
        ImGui::EndDisabled();
        ImGui::Checkbox("Custom relationship", &_custom);
        DrawOkCancelModal(
            [=]() {
                ExecuteAfterDraw<PrimCreateRelationship>(_sdfPrim, _relationName, _variability, _custom, _operation, _targetPath);
            },
            _relationName == "");
    }
    const char *DialogId() const override { return "Create relationship"; }

    const SdfPrimSpecHandle &_sdfPrim;
    std::string _relationName;
    std::string _targetPath;
    SdfListOpType _operation = SdfListOpTypeExplicit;
    SdfVariability _variability = SdfVariabilityVarying;
    bool _custom = false;
    bool _useSchema = true;
    std::vector<std::string> _allSchemaNames;
    std::vector<std::string>::iterator _selectedSchemaName = _allSchemaNames.end();
    std::vector<std::string> _allRelationshipNames;
    std::vector<std::string>::iterator _selectedRelationshipName = _allRelationshipNames.end();
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

/// Very basic ui to create a connection
struct CreateSdfAttributeConnectionDialog : public ModalDialog {

    CreateSdfAttributeConnectionDialog(SdfAttributeSpecHandle attribute) : _attribute(attribute){};
    ~CreateSdfAttributeConnectionDialog() override {}

    void Draw() override {
        if (!_attribute)
            return;
        ImGui::Text("Create connection for %s", _attribute->GetPath().GetString().c_str());
        if (ImGui::BeginCombo("Edit list", GetListEditorOperationName(_operation))) {
            for (int i = 0; i < GetListEditorOperationSize(); ++i) {
                if (ImGui::Selectable(GetListEditorOperationName(i), false)) {
                    _operation = static_cast<SdfListOpType>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::InputText("Path", &_connectionEndPoint);
        DrawOkCancelModal(
            [&]() { ExecuteAfterDraw<PrimCreateAttributeConnection>(_attribute, _operation, _connectionEndPoint); });
    }
    const char *DialogId() const override { return "Attribute connection"; }

    SdfAttributeSpecHandle _attribute;
    std::string _connectionEndPoint;
    SdfListOpType _operation = SdfListOpTypeExplicit;
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
template <typename T> static inline void DrawArrayEditorButton(T attribute) {
    if ((*attribute)->GetDefaultValue().IsArrayValued()) {
        if (ImGui::Button(ICON_FA_LIST)) {
            ExecuteAfterDraw<EditorSelectAttributePath>((*attribute)->GetPath());
        }
        ImGui::SameLine();
    }
}

inline SdfPathEditorProxy GetPathEditorProxy(SdfSpecHandle spec, TfToken field) {
#ifdef WIN32
    // Unfortunately on windows the SdfGetPathEditorProxy is not exposed
    // So the following code is a workaround
    // Only two calls for the moment, with the arguments:
    //attribute, SdfFieldKeys->ConnectionPaths
    //relation, SdfFieldKeys->TargetPaths
    if (spec->GetSpecType() == SdfSpecTypeAttribute && field == SdfFieldKeys->ConnectionPaths) {
        SdfAttributeSpecHandle attr = spec->GetLayer()->GetAttributeAtPath(spec->GetPath());
        return attr->GetConnectionPathList();
    } else if (spec->GetSpecType() == SdfSpecTypeRelationship && field == SdfFieldKeys->TargetPaths) {
        SdfRelationshipSpecHandle rel = spec->GetLayer()->GetRelationshipAtPath(spec->GetPath());
        return rel->GetTargetPathList();
    }
    return {}; // Shouldn't happen, if it does, add the case above
#else
return SdfGetPathEditorProxy(spec, field);
#endif
}

// This editor is specialized for list editor of tokens in the metadata.
// TODO: The code is really similar to DrawSdfPathListOneLinerEditor and ideally they should be unified.
static void DrawTfTokenListOneLinerEditor(SdfSpecHandle spec, TfToken field) {
    SdfTokenListOp proxy = spec->GetInfo(field).Get<SdfTokenListOp>();
    SdfListOpType currentList = GetEditListChoice(proxy);
    
    // Edit list chooser
    DrawEditListSmallButtonSelector(currentList, proxy);

    ImGui::SameLine();
    thread_local std::string itemsString; // avoid reallocating for every element at every frame
    itemsString.clear();
    for (const TfToken &item : GetSdfListOpItems(proxy, currentList)) {
        itemsString.append(item.GetString());
        itemsString.append(" "); // we don't care about the last space, it also helps the user adding a new item
    }

    ImGui::InputText("##EditListOneLineEditor", &itemsString);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::vector<std::string> newList = TfStringSplit(itemsString, " ");
        // TODO: should the following code go in a command ??
        std::function<void()> updateList = [=]() {
            if (spec) {
                SdfTokenListOp listOp = spec->GetInfo(field).Get<SdfTokenListOp>();
                SdfTokenListOp::ItemVector editList;
                for (const auto &path : newList) {
                    editList.push_back(TfToken(path));
                }
                SetSdfListOpItems(listOp, currentList, editList);
                spec->SetInfo(field, VtValue(listOp));
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(spec->GetLayer(), updateList);
    }
}

// This function is specialized for editing list of paths in a line editor
static void DrawSdfPathListOneLinerEditor(SdfSpecHandle spec, TfToken field) {
    SdfPathEditorProxy proxy = GetPathEditorProxy(spec, field);
    SdfListOpType currentList = GetEditListChoice(proxy);
    
    // Edit listop chooser
    DrawEditListSmallButtonSelector(currentList, proxy);

    ImGui::SameLine();
    thread_local std::string itemsString; // avoid reallocating
    itemsString.clear();
    for (const SdfPath &item : GetSdfListOpItems(proxy, currentList)) {
        itemsString.append(item.GetString());
        itemsString.append(" "); // we don't care about the last space, it also helps the user adding a new item
    }

    ImGui::InputText("##EditListOneLineEditor", &itemsString);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::vector<std::string> newList = TfStringSplit(itemsString, " ");
        // TODO: should the following code go in a command ??
        std::function<void()> updateList = [=]() {
            if (spec) {
                auto editorProxy = GetPathEditorProxy(spec, field);
                auto editList = GetSdfListOpItems(editorProxy, currentList);
                editList.clear();
                for (const auto &path : newList) {
                    editList.push_back(SdfPath(path));
                }
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(spec->GetLayer(), updateList);
    }
}

static void DrawAttributeRowPopupMenu(const SdfAttributeSpecHandle &attribute) {
    if (ImGui::MenuItem(ICON_FA_TRASH " Remove attribute")) {
        // attribute->GetLayer()->GetAttributeAtPath(attribute)
        SdfPrimSpecHandle primSpec = attribute->GetLayer()->GetPrimAtPath(attribute->GetOwner()->GetPath());
        ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath(attribute->GetPath()));
    }
    if (!attribute->HasConnectionPaths() && ImGui::MenuItem(ICON_FA_LINK " Create connection")) {
        DrawModalDialog<CreateSdfAttributeConnectionDialog>(attribute);
    }

    // Only if there are no default
    if (!attribute->HasDefaultValue() && ImGui::MenuItem(ICON_FA_PLUS " Create default value")) {
        std::function<void()> createDefaultValue = [=]() {
            if (attribute) {
                auto defaultValue = attribute->GetTypeName().GetDefaultValue();
                attribute->SetDefaultValue(defaultValue);
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(attribute->GetLayer(), createDefaultValue);
    }
    if (attribute->HasDefaultValue() && ImGui::MenuItem(ICON_FA_TRASH " Clear default value")) {
        std::function<void()> clearDefaultValue = [=]() {
            if (attribute) {
                attribute->ClearDefaultValue();
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(attribute->GetLayer(), clearDefaultValue);
    }

    if (ImGui::MenuItem(ICON_FA_KEY " Add key value")) {
        DrawTimeSampleCreationDialog(attribute->GetLayer(), attribute->GetPath());
    }
    
    // TODO: delete keys if it has keys
    
    if (ImGui::MenuItem(ICON_FA_COPY " Copy property")) {
        ExecuteAfterDraw<PropertyCopy>(static_cast<SdfPropertySpecHandle>(attribute));
    }

    if (ImGui::MenuItem(ICON_FA_COPY " Copy property path")) {
        ImGui::SetClipboardText(attribute->GetPath().GetString().c_str());
    }
}

struct AttributeRow {};
// TODO:
// ICON_FA_EDIT could be great for edit target
// ICON_FA_DRAW_POLYGON great for meshes
template <>
inline ScopedStyleColor GetRowStyle<AttributeRow>(const int rowId, const SdfAttributeSpecHandle &attribute,
                                                  const Selection &selection, const int &showWhat) {
    const bool selected = selection.IsSelected(attribute);
    ImVec4 colorSelected = selected ? ImVec4(ColorAttributeSelectedBg) : ImVec4(0.75, 0.60, 0.33, 0.2);
    return ScopedStyleColor(ImGuiCol_HeaderHovered, selected ? colorSelected : ImVec4(ColorTransparent), ImGuiCol_HeaderActive,
                            ImVec4(ColorTransparent), ImGuiCol_Header, colorSelected, ImGuiCol_Text,
                            ImVec4(ColorAttributeAuthored), ImGuiCol_Button, ImVec4(ColorTransparent), ImGuiCol_FrameBg,
                            ImVec4(ColorEditableWidgetBg));
}

template <>
inline void DrawFirstColumn<AttributeRow>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection,
                                          const int &showWhat) {
    ImGui::PushID(rowId);
    if (ImGui::Button(ICON_FA_TRASH)) {
        SdfPrimSpecHandle primSpec = attribute->GetLayer()->GetPrimAtPath(attribute->GetOwner()->GetPath());
        ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath(attribute->GetPath()));
    }
    ImGui::PopID();
};

template <>
inline void DrawSecondColumn<AttributeRow>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection,
                                           const int &showWhat) {
    // Still not sure we want to show the type at all or in the same column as the name
    ImGui::PushStyleColor(ImGuiCol_Text, attribute->HasDefaultValue() ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored));
    ImGui::Text(ICON_FA_FLASK);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const bool hasTimeSamples = attribute->GetLayer()->GetNumTimeSamplesForPath(attribute->GetPath()) != 0;
    ImGui::PushStyleColor(ImGuiCol_Text, hasTimeSamples ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored));
    ImGui::Text(ICON_FA_KEY);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, attribute->HasConnectionPaths() ? ImVec4(ColorAttributeAuthored) : ImVec4(ColorAttributeUnauthored));
    ImGui::Text(ICON_FA_LINK);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const std::string attributeText(attribute->GetName() + " (" + attribute->GetTypeName().GetAsToken().GetString() + ")");
    ImGui::Text("%s", attributeText.c_str());
    if (ImGui::BeginPopupContextItem(attributeText.c_str())) {
        DrawAttributeRowPopupMenu(attribute);
        ImGui::EndPopup();
    }
};

template <>
inline void DrawThirdColumn<AttributeRow>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection,
                                          const int &showWhat) {
    // Check what to show, this could be stored in a variable ... check imgui
    // For the mini buttons: ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::PushID(rowId);
    bool selected = selection.IsSelected(attribute);
    if (ImGui::Selectable("##select", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
        ExecuteAfterDraw<EditorSelectAttributePath>(attribute->GetPath());
    }
    ImGui::SameLine();
    if (attribute->HasDefaultValue()) {
        // Show Default value if any
        ImGui::PushItemWidth(-FLT_MIN);
        VtValue modified = DrawVtValue("##Default", attribute->GetDefaultValue());
        if (modified != VtValue()) {
            ExecuteAfterDraw(&SdfPropertySpec::SetDefaultValue, attribute, modified);
        }
    }

    if (attribute->HasConnectionPaths()) {
        ScopedStyleColor connectionColor(ImGuiCol_Text, ImVec4(ColorAttributeConnection));
        SdfConnectionsProxy connections = attribute->GetConnectionPathList();
        DrawSdfPathListOneLinerEditor(attribute, SdfFieldKeys->ConnectionPaths);
    }
    ImGui::PopID();
};

void DrawPrimSpecAttributes(const SdfPrimSpecHandle &primSpec, const Selection &selection) {
    if (!primSpec)
        return;

    const auto &attributes = primSpec->GetAttributes();
    if (attributes.empty())
        return;
    if (ImGui::CollapsingHeader("Attributes", ImGuiTreeNodeFlags_DefaultOpen)) {
        int rowId = 0;
        if (BeginThreeColumnsTable("##DrawPrimSpecAttributes")) {
            SetupThreeColumnsTable(false, "", "Attribute", "");
            ImGui::PushID(primSpec->GetPath().GetHash());
            // the third column allows to show different attribute properties:
            // Default value, keyed values or connections (and summary ??)
            // int showWhat = DrawValueColumnSelector();
            int showWhat = 0;
            for (const SdfAttributeSpecHandle &attribute : attributes) {
                DrawThreeColumnsRow<AttributeRow>(rowId++, attribute, selection, showWhat);
            }
            ImGui::PopID();
            ImGui::EndTable();
        }
    }
}

struct RelationRow {};
template <>
inline void DrawFirstColumn<RelationRow>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                         const SdfRelationshipSpecHandle &relation) {
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath(relation->GetPath()));
    }
};
template <>
inline void DrawSecondColumn<RelationRow>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                          const SdfRelationshipSpecHandle &relation) {
    ImGui::Text("%s", relation->GetName().c_str());
};
template <>
inline void DrawThirdColumn<RelationRow>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                         const SdfRelationshipSpecHandle &relation) {
    ImGui::PushItemWidth(-FLT_MIN);
    DrawSdfPathListOneLinerEditor(relation, SdfFieldKeys->TargetPaths);
};

void DrawPrimSpecRelations(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    const auto &relationships = primSpec->GetRelationships();
    if (relationships.empty())
        return;
    if (ImGui::CollapsingHeader("Relations", ImGuiTreeNodeFlags_DefaultOpen)) {
        int rowId = 0;
        if (BeginThreeColumnsTable("##DrawPrimSpecRelations")) {
            SetupThreeColumnsTable(false, "", "Relations", "");
            auto relations = primSpec->GetRelationships();
            for (const SdfRelationshipSpecHandle &relation : relations) {
                ImGui::PushID(relation->GetPath().GetHash());
                DrawThreeColumnsRow<RelationRow>(rowId++, primSpec, relation);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

struct ApiSchemaRow {};
template <> inline bool HasEdits<ApiSchemaRow>(const SdfPrimSpecHandle &prim) { return prim->HasInfo(UsdTokens->apiSchemas); }
template <> inline void DrawFirstColumn<ApiSchemaRow>(const int rowId, const SdfPrimSpecHandle &primSpec) {
    ImGui::PushID(rowId);
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfPrimSpec::ClearInfo, primSpec, UsdTokens->apiSchemas);
    }
    ImGui::PopID();
};
template <> inline void DrawSecondColumn<ApiSchemaRow>(const int rowId, const SdfPrimSpecHandle &primSpec) {
    ImGui::Text("API Schemas");
};
template <> inline void DrawThirdColumn<ApiSchemaRow>(const int rowId, const SdfPrimSpecHandle &primSpec) {
    ImGui::PushItemWidth(-FLT_MIN);
    DrawTfTokenListOneLinerEditor(primSpec, UsdTokens->apiSchemas);
};

#define GENERATE_FIELD(ClassName_, FieldName_, DrawFunction_)                                                                    \
    struct ClassName_ {                                                                                                          \
        static constexpr const char *fieldName = FieldName_;                                                                     \
    };                                                                                                                           \
    template <> inline void DrawThirdColumn<ClassName_>(const int rowId, const SdfPrimSpecHandle &primSpec) {                    \
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);                                                               \
        DrawFunction_(primSpec);                                                                                                 \
    }

#define GENERATE_FIELD_WITH_BUTTON(ClassName_, Token_, FieldName_, DrawFunction_)                                                \
    GENERATE_FIELD(ClassName_, FieldName_, DrawFunction_);                                                                       \
    template <> inline bool HasEdits<ClassName_>(const SdfPrimSpecHandle &prim) { return prim->HasField(Token_); }               \
    template <> inline void DrawFirstColumn<ClassName_>(const int rowId, const SdfPrimSpecHandle &primSpec) {                    \
        ImGui::PushID(rowId);                                                                                                    \
        if (ImGui::Button(ICON_FA_TRASH) && HasEdits<ClassName_>(primSpec)) {                                                    \
            ExecuteAfterDraw(&SdfPrimSpec::ClearField, primSpec, Token_);                                                        \
        }                                                                                                                        \
        ImGui::PopID();                                                                                                          \
    }

GENERATE_FIELD(Specifier, "Specifier", DrawPrimSpecifier);
GENERATE_FIELD(PrimType, "Type", DrawPrimType);
GENERATE_FIELD(PrimName, "Name", DrawPrimName);
GENERATE_FIELD_WITH_BUTTON(PrimKind, SdfFieldKeys->Kind, "Kind", DrawPrimKind);
GENERATE_FIELD_WITH_BUTTON(PrimActive, SdfFieldKeys->Active, "Active", DrawPrimActive);
GENERATE_FIELD_WITH_BUTTON(PrimInstanceable, SdfFieldKeys->Instanceable, "Instanceable", DrawPrimInstanceable);
GENERATE_FIELD_WITH_BUTTON(PrimHidden, SdfFieldKeys->Hidden, "Hidden", DrawPrimHidden);

void DrawPrimSpecMetadata(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec->GetPath().IsPrimVariantSelectionPath()) {
        if (ImGui::CollapsingHeader("Core Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
            int rowId = 0;
            if (BeginThreeColumnsTable("##DrawPrimSpecMetadata")) {
                SetupThreeColumnsTable(false, "", "Metadata", "Value");
                ImGui::PushID(primSpec->GetPath().GetHash());
                DrawThreeColumnsRow<Specifier>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimType>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimName>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimKind>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimActive>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimInstanceable>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimHidden>(rowId++, primSpec);
                DrawThreeColumnsRow<ApiSchemaRow>(rowId++, primSpec);
                DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, primSpec, SdfFieldKeys->CustomData);
                DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, primSpec, SdfFieldKeys->AssetInfo);
                ImGui::PopID();
                EndThreeColumnsTable();
            }
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
        if (ImGui::BeginMenu("New", enabled)) {
            DrawPrimCreateCompositionMenu(primSpec);
            ImGui::Separator();
            DrawPrimCreatePropertyMenu(primSpec); // attributes and relation
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit", enabled)) {
            if (ImGui::MenuItem("Paste")) {
                ExecuteAfterDraw<PropertyPaste>(primSpec);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void DrawSdfPrimEditor(const SdfPrimSpecHandle &primSpec, const Selection &selection) {
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
    DrawPrimCompositions(primSpec);
    DrawPrimVariants(primSpec);
    DrawPrimSpecAttributes(primSpec, selection);
    DrawPrimSpecRelations(primSpec);
    ImGui::EndChild();
    if (ImGui::IsItemHovered()) {
        const SdfPath &selectedProperty = selection.GetAnchorPropertyPath(primSpec->GetLayer());
        if (selectedProperty != SdfPath()) {
            SdfPropertySpecHandle selectedPropertySpec = primSpec->GetLayer()->GetPropertyAtPath(selectedProperty);
            AddShortcut<PropertyCopy, ImGuiKey_LeftCtrl, ImGuiKey_C>(selectedPropertySpec);
        }
        AddShortcut<PropertyPaste, ImGuiKey_LeftCtrl, ImGuiKey_V>(primSpec);
    }
}
