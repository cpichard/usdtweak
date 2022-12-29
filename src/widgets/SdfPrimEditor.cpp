
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
#include "TableLayouts.h"
#include "Shortcuts.h"
#include "VtDictionaryEditor.h"

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
        ImGui::Checkbox("Create default value", &_createDefault);
        DrawOkCancelModal(
            [&]() { ExecuteAfterDraw<PrimCreateAttribute>(_sdfPrim, _attributeName, _typeName, _variability, _custom, _createDefault); });
    }
    const char *DialogId() const override { return "Create attribute"; }

    const SdfPrimSpecHandle &_sdfPrim;
    std::string _attributeName;
    SdfVariability _variability = SdfVariabilityVarying;
    SdfValueTypeName _typeName = SdfValueTypeNames->Bool;
    bool _custom = true;
    bool _createDefault = false;
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

/// Very basic ui to create a connection
struct CreateSdfAttributeConnectionDialog : public ModalDialog {

    CreateSdfAttributeConnectionDialog(SdfAttributeSpecHandle attribute) : _attribute(attribute){};
    ~CreateSdfAttributeConnectionDialog() override {}

    void Draw() override {
        if(!_attribute) return;
        ImGui::Text("Create connection for %s", _attribute->GetPath().GetString().c_str());
        if (ImGui::BeginCombo("Edit list", GetListEditorOperationName(_operation))) {
            for (int i = 0; i < GetListEditorOperationSize(); ++i) {
                if (ImGui::Selectable(GetListEditorOperationName(i), false)) {
                    _operation = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::InputText("Path", &_connectionEndPoint);
        DrawOkCancelModal([&]() {
            ExecuteAfterDraw<PrimCreateAttributeConnection>(_attribute, _operation, _connectionEndPoint);
        });
    }
    const char *DialogId() const override { return "Attribute connection"; }

    SdfAttributeSpecHandle _attribute;
    std::string _connectionEndPoint;
    int _operation = 0;
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

// TODO: draw connections as well
// TODO: show if they have default value, keys, connections
// TODO: show the type as well
// TODO: Copy property name / Copy property value
//void DrawPrimSpecAttributes(const SdfPrimSpecHandle &primSpec, const Selection &selection) {
//    if (!primSpec)
//        return;
//
//    int deleteButtonCounter = 0;
//    const auto &attributes = primSpec->GetAttributes();
//    if (attributes.empty())
//        return;
//    static ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
//    if (ImGui::BeginTable("##DrawPrimSpecAttributes", 4, tableFlags)) {
//        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 48); // 24 => size of the mini button
//        ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
//        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
//
//        ImGui::TableHeadersRow();
//
//        TF_FOR_ALL(attribute, attributes) {
//            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
//
//            ImGui::TableNextRow();
//
//            // MiniButton
//            ImGui::TableSetColumnIndex(0);
//            ImGui::PushID(deleteButtonCounter++);
//
//            if (ImGui::Button(ICON_FA_TRASH)) {
//                ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath((*attribute)->GetPath()));
//            }
//            ImGui::SameLine();
//            if (ImGui::Button("S")) {
//                ExecuteAfterDraw<EditorSelectAttributePath>((*attribute)->GetPath());
//            }
//            ImGui::SameLine();
//            if (selection.IsSelected(*attribute)) {
//                ImGui::Text("Selected");
//            }
//            ImGui::PopID();
//
//            // Name of the parameter
//            ImGui::TableSetColumnIndex(1);
//            SdfTimeSampleMap timeSamples = (*attribute)->GetTimeSampleMap();
//            if (timeSamples.empty())
//                nodeFlags |= ImGuiTreeNodeFlags_Leaf;
//            bool unfolded = ImGui::TreeNodeEx((*attribute)->GetName().c_str(), nodeFlags, "%s", (*attribute)->GetName().c_str());
//            if ((*attribute)->HasDefaultValue()) { // If closed show the default value
//                ImGui::TableSetColumnIndex(2);
//                const std::string defaultValueLabel = "Default";
//                ImGui::Text("%s", defaultValueLabel.c_str());
//
//                ImGui::TableSetColumnIndex(3);
//                DrawArrayEditorButton(attribute);
//                ImGui::PushItemWidth(-FLT_MIN);
//                VtValue modified = DrawVtValue(defaultValueLabel, (*attribute)->GetDefaultValue());
//                if (modified != VtValue()) {
//                    ExecuteAfterDraw(&SdfPropertySpec::SetDefaultValue, *attribute, modified);
//                }
//                ImGui::PopItemWidth();
//            }
//
//            if (unfolded) {
//                TF_FOR_ALL(sample, timeSamples) { // Samples
//                    ImGui::TableNextRow();
//                    // Mini button
//                    ImGui::TableSetColumnIndex(1);
//                    ImGui::PushID(deleteButtonCounter++);
//                    if (ImGui::Button(ICON_FA_TRASH)) {
//                        ExecuteAfterDraw(&SdfLayer::EraseTimeSample, primSpec->GetLayer(), (*attribute)->GetPath(),
//                                         sample->first);
//                    }
//                    ImGui::PopID();
//                    // Time: TODO edit time ?
//                    ImGui::TableSetColumnIndex(2);
//                    std::string sampleValueLabel = std::to_string(sample->first);
//                    ImGui::Text("%s", sampleValueLabel.c_str());
//                    // Value
//                    ImGui::TableSetColumnIndex(3);
//                    DrawArrayEditorButton(attribute);
//                    ImGui::PushItemWidth(-FLT_MIN);
//                    VtValue modified = DrawVtValue(sampleValueLabel, sample->second);
//                    if (modified != VtValue()) {
//                        ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, primSpec->GetLayer(), (*attribute)->GetPath(),
//                                         sample->first, modified);
//                    }
//                    ImGui::PopItemWidth();
//                }
//                ImGui::TreePop();
//            }
//        }
//        ImGui::EndTable();
//    }
//}
static void DrawAttributeConnectionEditListItem(const char *operation, const SdfPath &path, int &id, const SdfAttributeSpecHandle &attribute) {
//    ImGui::Selectable(path.GetText());
//    if (ImGui::BeginPopupContextItem(nullptr)) {
//        if(ImGui::MenuItem("Remove connection")) {
//            ExecuteAfterDraw<PrimRemoveAttributeConnection>(attribute, path);
//        }
//        ImGui::EndPopup();
//    }
    ImGui::PushID(path.GetText());
    ImGui::Text("%s", operation);
    ImGui::SameLine();
    std::string newPathStr = path.GetString();
    ImGui::InputText("###AttributeConnection", &newPathStr);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if(newPathStr.empty()) {
            std::function<void()> removeItem = [=]() {
                if (attribute) {
                    attribute->GetConnectionPathList().RemoveItemEdits(path);
                }
            };
            ExecuteAfterDraw<UsdFunctionCall>(attribute->GetLayer(), removeItem);
        } else {
            std::function<void()> replaceItemEdits = [=]() {
                if (attribute) {
                    attribute->GetConnectionPathList().ReplaceItemEdits(path, SdfPath(newPathStr));
                }
            };
            ExecuteAfterDraw<UsdFunctionCall>(attribute->GetLayer(), replaceItemEdits);
        }
    }
    // TODO we might also want to be able to edit what the connection is pointing
    // ImGui::Text("%s", path.GetText());
    ImGui::PopID();
}
struct AttributeField{};
// TODO:
// ICON_FA_EDIT could be great for edit target
// ICON_FA_DRAW_POLYGON great for meshes
template <> inline ScopedStyleColor GetRowStyle<AttributeField>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection, const int &showWhat) {
    const bool selected = selection.IsSelected(attribute);
    ImVec4 colorSelected = selected ? ImVec4(ColorAttributeSelectedBg) : ImVec4(0.75, 0.60, 0.33, 0.2);
    return ScopedStyleColor(ImGuiCol_HeaderHovered, selected ? colorSelected : ImVec4(ColorTransparent),
                                 ImGuiCol_HeaderActive, ImVec4(ColorTransparent), ImGuiCol_Header, colorSelected, ImGuiCol_Text, ImVec4(ColorAttributeAuthored),
                                 ImGuiCol_Button, ImVec4(ColorTransparent),
                            ImGuiCol_FrameBg, ImVec4(ColorEditableWidgetBg));
}


template <>
inline void DrawFirstColumn<AttributeField>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection, const int &showWhat) {
    ImGui::PushID(rowId);
 
    ImGui::Button(ICON_FA_CARET_SQUARE_DOWN);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        if (ImGui::MenuItem(ICON_FA_TRASH " Remove attribute")) {
            //attribute->GetLayer()->GetAttributeAtPath(attribute)
            SdfPrimSpecHandle primSpec = attribute->GetLayer()->GetPrimAtPath(attribute->GetOwner()->GetPath());
            ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath(attribute->GetPath()));
        }
        if (ImGui::MenuItem(ICON_FA_LINK " Create connection")) {
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
            //DrawModalDialog<>(attribute);
        }
        
        if (ImGui::MenuItem(ICON_FA_COPY " Copy property")) {
            ExecuteAfterDraw<PropertyCopy>(static_cast<SdfPropertySpecHandle>(attribute));
        }
        
        if (ImGui::MenuItem(ICON_FA_COPY " Copy property path")) {
            ImGui::SetClipboardText(attribute->GetPath().GetString().c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
};

template <>
inline void DrawSecondColumn<AttributeField>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection, const int &showWhat) {
    // Still not sure we want to show the type at all or in the same column as the name
    ImGui::Text("%s (%s)", attribute->GetName().c_str(), attribute->GetTypeName().GetAsToken().GetText());
};
 
template <>
inline void DrawThirdColumn<AttributeField>(const int rowId, const SdfAttributeSpecHandle &attribute, const Selection &selection, const int &showWhat) {
    //ImGui::Button(ICON_FA_KEY); // menu to show stuff ??? -> do we want to show more stuff ???
    //ImGui::SameLine();
    // Check what to show, this could be stored in a variable ... check imgui
    ImGui::PushID(rowId);
    bool selected = selection.IsSelected(attribute);
    if(ImGui::Selectable("##select", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
        ExecuteAfterDraw<EditorSelectAttributePath>(attribute->GetPath());
    }
    ImGui::SameLine();
    
  //  SdfTimeSampleMap timeSamples = attribute->GetTimeSampleMap();
//    ImGuiContext &g = *GImGui;
//    ImGuiWindow *window = g.CurrentWindow;
//    ImGuiStorage *storage = window->DC.StateStorage;
//    ImGui::PushID("timeSample");
//    const auto key = ImGui::GetItemID();
//    int what = storage->GetInt(key, 0);
//    ImGui::PopID(); // Timesample
//    if ( (!timeSamples.empty()||attribute->HasDefaultValue())
//        && attribute->GetVariability() == SdfVariability::SdfVariabilityVarying ){
//        //ImGui::SmallButton(ICON_FA_KEY);
//        if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
//            if (attribute->HasDefaultValue()) {
//                ImGui::MenuItem("default");
//            }
//             TF_FOR_ALL(sample, timeSamples) { // Samples
//
//                std::string sampleValueLabel = std::to_string(sample->first);
//                 ImGui::MenuItem(sampleValueLabel.c_str());
//
//            }
//            ImGui::EndPopup();
//        }
//
//        if(ImGui::ArrowButton("##left", ImGuiDir_Left)) {
//            what -= 1;
//            storage->SetInt(key,what);
//        }
//        ImGui::SameLine();
//        ImGui::Text(std::to_string(what).c_str());
//        ImGui::SameLine();
//        if(ImGui::ArrowButton("##right", ImGuiDir_Right)) {
//            what += 1;
//            storage->SetInt(key,what);
//        }
//        ImGui::SameLine();
//    }
    
    
   // ImGui::SameLine();
    if (attribute->HasDefaultValue()) {
        // Show Default value if any
        ImGui::PushItemWidth(-FLT_MIN);
        VtValue modified = DrawVtValue("##Default", attribute->GetDefaultValue());
        if (modified != VtValue()) {
            ExecuteAfterDraw(&SdfPropertySpec::SetDefaultValue, attribute, modified);
        }
    }
    

    
//    if (!timeSamples.empty()) {
//        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
//        //nodeFlags |= ImGuiTreeNodeFlags_Leaf;
//        bool unfolded = ImGui::TreeNodeEx(attribute->GetName().c_str(), nodeFlags, "%s", attribute->GetName().c_str());
//        if (unfolded) {
//            TF_FOR_ALL(sample, timeSamples) { // Samples
//
//                std::string sampleValueLabel = std::to_string(sample->first);
//                VtValue modified = DrawVtValue(sampleValueLabel, sample->second);
//                if (modified != VtValue()) {
//                    ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, attribute->GetLayer(), attribute->GetPath(),
//                                     sample->first, modified);
//                }
//            }
//            ImGui::TreePop();
//        }
//    }
    //if (attribute->Time) {

      //  SdfTimeSampleMap timeSamples = attribute->GetTimeSampleMap();
        
    //}
    

    if (attribute->HasConnectionPaths()) {
        ImGui::SmallButton(ICON_FA_LINK);
        ImGui::SameLine();
        SdfConnectionsProxy connections = attribute->GetConnectionPathList();
        // SdfConnectionProxy is a SdfListEditorProxy
        // What should happen when we have multiple connections ?
        int connectionId = 0;
        ScopedStyleColor connectionColor(ImGuiCol_Text, ImVec4(ColorAttributeConnection));
        IterateListEditorItems(attribute->GetConnectionPathList(), DrawAttributeConnectionEditListItem, connectionId, attribute);
    }
    ImGui::PopID();
};

// ComboBox inserted in a table header.
static int DrawValueColumnSelector() {
    static int showWhat = 0;
    const char *choices[3] = {"Default value", "Keyed values", "Connections"};
    ImGui::TableSetColumnIndex(2);
    ImGui::SameLine();
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0, 0.0, 0.0, 0.0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0, 0.0));
    if (ImGui::BeginCombo("Show", choices[showWhat], ImGuiComboFlags_NoArrowButton)) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(choices[i])) {
                showWhat = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return showWhat;
}


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
            // the third column allows to show different attribute properties:
            // Default value, keyed values or connections (and summary ??)
            //int showWhat = DrawValueColumnSelector();
            int showWhat = 0;
            for (const SdfAttributeSpecHandle &attribute : attributes) {
                DrawThreeColumnsRow<AttributeField>(rowId++, attribute, selection, showWhat);
            }
            ImGui::EndTable();
        }
    }
}



static void DrawRelationshipEditListItem(const char *operation, const SdfPath &path, int &id, const SdfPrimSpecHandle &primSpec,
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
inline void DrawSecondColumn<RelationField>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                         const SdfRelationshipSpecHandle &relation) {
    ImGui::Text("%s", relation->GetName().c_str());
};
template <>
inline void DrawThirdColumn<RelationField>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                          const SdfRelationshipSpecHandle &relation) {
    IterateListEditorItems(relation->GetTargetPathList(), DrawRelationshipEditListItem, rowId, primSpec, relation->GetPath());
};
template <>
inline void DrawFirstColumn<RelationField>(const int rowId, const SdfPrimSpecHandle &primSpec,
                                           const SdfRelationshipSpecHandle &relation) {
    if (ImGui::Button(ICON_FA_TRASH)) {
        ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec, primSpec->GetPropertyAtPath(relation->GetPath()));
    }
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
                DrawThreeColumnsRow<RelationField>(rowId, primSpec, relation);
            }
            ImGui::EndTable();
        }
    }
}


#define GENERATE_FIELD(ClassName_, FieldName_, DrawFunction_)                                                                    \
    struct ClassName_ {                                                                                                          \
        static constexpr const char *fieldName = FieldName_;                                                                     \
    };                                                                                                                           \
    template <> inline void DrawThirdColumn<ClassName_>(const int rowId, const SdfPrimSpecHandle &primSpec) {                     \
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
        if (ImGui::CollapsingHeader("Core Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
            int rowId = 0;
            if (BeginThreeColumnsTable("##DrawPrimSpecMetadata")) {
                SetupThreeColumnsTable(false, "", "Metadata", "Value");
                DrawThreeColumnsRow<Specifier>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimType>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimName>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimKind>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimActive>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimInstanceable>(rowId++, primSpec);
                DrawThreeColumnsRow<PrimHidden>(rowId++, primSpec);
                DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, primSpec, SdfFieldKeys->CustomData);
                DrawThreeColumnsDictionaryEditor<SdfPrimSpec>(rowId, primSpec, SdfFieldKeys->AssetInfo);
                EndThreeColumnsTable();
            }
            ImGui::Separator();
        }
    }
}



static void DrawPrimAssetInfo(const SdfPrimSpecHandle prim) {
    if (!prim)
        return;
    const auto &assetInfo = prim->GetAssetInfo();
    if (!assetInfo.empty()) {
        if (ImGui::CollapsingHeader("Asset Info", ImGuiTreeNodeFlags_DefaultOpen)) { // This should really go in the metadata header
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
        DrawPrimAssetInfo(primSpec);

    DrawPrimCompositions(primSpec);
    DrawPrimVariants(primSpec);
    DrawPrimSpecAttributes(primSpec, selection);
    DrawPrimSpecRelations(primSpec);
    ImGui::EndChild();
    if (ImGui::IsItemHovered()) {
        const SdfPath &selectedProperty = selection.GetAnchorPropertyPath(primSpec->GetLayer());
        if (selectedProperty!=SdfPath()) {
            SdfPropertySpecHandle selectedPropertySpec = primSpec->GetLayer()->GetPropertyAtPath(selectedProperty);
            AddShortcut<PropertyCopy, ImGuiKey_LeftCtrl, ImGuiKey_C>(selectedPropertySpec);
        }
        AddShortcut<PropertyPaste, ImGuiKey_LeftCtrl, ImGuiKey_V>(primSpec);
    }
}
