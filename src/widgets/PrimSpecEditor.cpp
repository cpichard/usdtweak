
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

#include "Gui.h"
#include "PrimSpecEditor.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "Commands.h"
#include "Constants.h"
#include "ValueEditor.h"
#include "LayerEditor.h"

//
static const char *ArcAppendChar = "Append";
static const char *ArcAddChar = "Add";
static const char *ArcPrependChar = "Prepend";
static const char *ArcDeleteChar = "Delete";
static const char *ArcExplicitChar = "Explicit";
static const char *ArcOrderedChar = "Ordered";

/// Should that move in Common.h ???
std::array<char, TextEditBufferSize> CreateEditBufferFor(const std::string &str) {
    auto strEnd = str.size() < (TextEditBufferSize-1) ? str.cend() : str.cbegin() + (TextEditBufferSize-1);
    std::array<char, TextEditBufferSize> buffer{ 0 };
    std::copy(str.cbegin(), strEnd, buffer.begin());
    return buffer; // Should construct in place with copy ellision
}

// TODO: add all the composition arcs
struct EditReferences : public ModalDialog {

    EditReferences(SdfPrimSpecHandle &primSpec) : primSpec(primSpec), operation(0), composition(0){};

    ~EditReferences() override {}

    void Draw() override {
        if (!primSpec) {
            CloseModal();
            return;
        }
        ImGui::Text("Select file");
        DrawFileBrowser();
        if (ImGui::Button("Close")) {
            CloseModal();
        }

        if (ImGui::Button("Ok")) {
            std::string reference = GetFileBrowserFilePath();
            ExecuteAfterDraw<PrimAddReference>(primSpec, reference);
            CloseModal();
        }
    }

    const char *DialogId() const override { return "Edit Prim References"; }
    SdfPrimSpecHandle primSpec;
    int operation;
    int composition;
};

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

void DrawPrimInstanceable(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    bool isInstanceable = primSpec->GetInstanceable();
    if (ImGui::Checkbox("Instanceable", &isInstanceable)) {
        ExecuteAfterDraw(&SdfPrimSpec::SetInstanceable, primSpec, isInstanceable);
    }
    if (primSpec->HasInstanceable()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            ExecuteAfterDraw(&SdfPrimSpec::ClearInstanceable, primSpec);
        }
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
    if (primSpec->HasActive()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            ExecuteAfterDraw(&SdfPrimSpec::ClearActive, primSpec);
        }
    }
}

void DrawPrimName(SdfPrimSpecHandle &primSpec) {
    auto nameBuffer = CreateEditBufferFor(primSpec->GetName());
    ImGui::InputText("Prim Name", nameBuffer.data(), nameBuffer.size());
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

/// Draw a prim type name combo
void DrawPrimType(SdfPrimSpecHandle &primSpec, ImGuiComboFlags comboFlags) {

    /// TODO reset to none as well
    /// TODO: look at: https://github.com/ocornut/imgui/issues/282
    // TODO: get all types, ATM the function GetAllTypes is not resolved at link time
    // auto allTypes = primSpec->GetSchema().GetAllTypes();
    // They are also registered in "registry.usda"
    const char *classes[] = {"Scope",      "Xform",        "Cube",     "Sphere",         "Cylinder",
                             "Capsule",    "Cone",         "Camera",   "PointInstancer", "Mesh",
                             "GeomSubset", "DistantLight", "Material", "Shader", "BlendShape"};

    const TfToken &typeName = primSpec->GetTypeName() == SdfTokens->AnyTypeToken ? TfToken() : primSpec->GetTypeName();
    const char *currentItem = typeName.GetString().c_str();

    if (ImGui::BeginCombo("Prim Type", currentItem, comboFlags)) {
        for (int n = 0; n < IM_ARRAYSIZE(classes); n++) {
            bool isSelected = strcmp(currentItem, classes[n]) == 0;
            if (ImGui::Selectable(classes[n], isSelected)) {
                currentItem = classes[n];
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }

        if (currentItem && primSpec->GetTypeName() != currentItem) {
            if (currentItem=="") {
                ExecuteAfterDraw(&SdfPrimSpec::SetTypeName, primSpec, SdfTokens->AnyTypeToken);
            } else {
                ExecuteAfterDraw(&SdfPrimSpec::SetTypeName, primSpec, currentItem);
            }

        }
        ImGui::EndCombo();
    }
}

static inline const char *GetPathFrom(const SdfPath &value) { return value.GetString().c_str(); }
static inline const char *GetPathFrom(const SdfReference &value) { return value.GetAssetPath().c_str(); }
static inline const char *GetPathFrom(const SdfPayload &value) { return value.GetAssetPath().c_str(); }

// NOTE: there might be a better way of removing reference, not keeping the index but a copy of value_type
template <typename GetListFuncT, typename GetItemsFuncT>
void PrimSpecRemoveArc(SdfLayerHandle layer, SdfPath primSpecPath, GetListFuncT listFunc, GetItemsFuncT itemsFunc, size_t index) {
    if (layer) {
        auto primSpec = layer->GetPrimAtPath(primSpecPath);
        auto arcList = std::bind(listFunc, get_pointer(primSpec))();
        auto items = std::bind(itemsFunc, &arcList)();
        if (index<items.size()){
            arcList.RemoveItemEdits(items[index]);
        }
    }
}

template <typename GetListFuncT, typename GetItemsFuncT>
void DrawCompositionArcItems(const SdfPrimSpecHandle &primSpec, const char *operation,
                             const char *compositionType, GetListFuncT listFunc, GetItemsFuncT itemsFunc) {
    auto path = primSpec->GetPath();
    auto arcList = std::bind(listFunc, get_pointer(primSpec))();
    auto items = std::bind(itemsFunc,  &arcList)();
    size_t index = 0;
    for (const auto &ref : items) {
        // TODO: check if the variant code below would be still useful
        auto variantSelection = path.GetVariantSelection();
        ImGui::Text("%s %s", variantSelection.first.c_str(), variantSelection.second.c_str());
        ImGui::SameLine();
        ImGui::Text("%s %s", operation, compositionType);
        ImGui::SameLine();
        ImGui::TextUnformatted(GetPathFrom(ref));
        if (ImGui::BeginPopupContextItem(GetPathFrom(ref))) {
            if (ImGui::MenuItem("Remove reference")) {
                std::function<void()> deferedRemoveArc = [=] () {
                    PrimSpecRemoveArc(primSpec->GetLayer(), path, listFunc, itemsFunc, index);
                };
                ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), deferedRemoveArc);
             }
            //if (ImGui::MenuItem("Copy path")) {
            //    // TODO
            // }
            //if (ImGui::MenuItem("Open as stage")) {
            //    // TODO
            // }
            ImGui::EndPopup();
        }

        // TODO popup menu with JumpTo, Delete, Replace, etc
        index++;
    }
}

template <typename GetListFuncT>
void DrawCompositionArcOperations(const SdfPrimSpecHandle &primSpec, GetListFuncT listFunc,
                                  const char *compositionType) {
    using SdfListT = decltype(std::bind(listFunc, get_pointer(primSpec))());
    DrawCompositionArcItems(primSpec, ArcAppendChar, compositionType, listFunc, &SdfListT::GetAppendedItems);
    DrawCompositionArcItems(primSpec, ArcAddChar, compositionType, listFunc, &SdfListT::GetAddedItems);
    DrawCompositionArcItems(primSpec, ArcPrependChar, compositionType, listFunc, &SdfListT::GetPrependedItems);
    DrawCompositionArcItems(primSpec, ArcDeleteChar, compositionType, listFunc, &SdfListT::GetDeletedItems);
    DrawCompositionArcItems(primSpec, ArcExplicitChar, compositionType, listFunc, &SdfListT::GetExplicitItems);
    DrawCompositionArcItems(primSpec, ArcOrderedChar, compositionType, listFunc, &SdfListT::GetOrderedItems);
}

/// Draw all compositions in one big list
void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec) {
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetPayloadList, "Payload");
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetInheritPathList, "Inherit");
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetSpecializesList, "Specialize");
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetReferenceList, "Reference");
}

void DrawPrimCompositionSummary(SdfPrimSpecHandle &primSpec) {
    if (PrimHasComposition(primSpec)) {
        DrawPrimCompositions(primSpec);
        const auto &pathStr = primSpec->GetPath().GetString();
        SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
        TF_FOR_ALL(varSetIt, variantSetMap) {
            const SdfVariantSetSpecHandle &varSetSpec = varSetIt->second;
            const SdfVariantSpecHandleVector &variants = varSetSpec->GetVariantList();
            TF_FOR_ALL(varIt, variants) {
                const SdfPrimSpecHandle &variantSpec = (*varIt)->GetPrimSpec();
                DrawPrimCompositions(variantSpec);
            }
        }
    }
}

static bool HasComposition(const SdfPrimSpecHandle &primSpec) {
    return primSpec->HasReferences()
        || primSpec->HasPayloads()
        || primSpec->HasInheritPaths()
        || primSpec->HasSpecializes();
}

/// Returns if the prim has references, checking the variants
bool PrimHasComposition(const SdfPrimSpecHandle &primSpec, bool checkVariants) {

    if (HasComposition(primSpec)){
        return true;
    }

    if (checkVariants) {
        SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
        TF_FOR_ALL(varSetIt, variantSetMap) {
            const SdfVariantSetSpecHandle &varSetSpec = varSetIt->second;
            const SdfVariantSpecHandleVector &variants = varSetSpec->GetVariantList();
            TF_FOR_ALL(varIt, variants) {
                return PrimHasComposition((*varIt)->GetPrimSpec());
            }
        }
    }
    return false;
}


void DrawPrimCompositionPopupMenu(SdfPrimSpecHandle &primSpec) {

    if (ImGui::BeginMenu(ArcAppendChar)) {
        ImGui::MenuItem("Payload");
        ImGui::MenuItem("Inherit");
        ImGui::MenuItem("Specialize");
        ImGui::MenuItem("Reference");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ArcPrependChar)) {
        ImGui::MenuItem("Payload");
        ImGui::MenuItem("Inherit");
        ImGui::MenuItem("Specialize");
        ImGui::MenuItem("Reference");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ArcAddChar)) {
        ImGui::MenuItem("Payload");
        ImGui::MenuItem("Inherit");
        ImGui::MenuItem("Specialize");
        if (ImGui::MenuItem("Reference")) {
            // TODO  TriggerOpenModal<EditReferences>(primSpec, ArcAddChar, ArcPayload);
            DrawModalDialog<EditReferences>(primSpec);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ArcDeleteChar)) {
        ImGui::MenuItem("Payload");
        ImGui::MenuItem("Inherit");
        ImGui::MenuItem("Specialize");
        ImGui::MenuItem("Reference");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ArcExplicitChar)) {
        ImGui::MenuItem("Payload");
        ImGui::MenuItem("Inherit");
        ImGui::MenuItem("Specialize");
        ImGui::MenuItem("Reference");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ArcOrderedChar)) {
        ImGui::MenuItem("Payload");
        ImGui::MenuItem("Inherit");
        ImGui::MenuItem("Specialize");
        ImGui::MenuItem("Reference");
        ImGui::EndMenu();
    }
}

void DrawPrimCompositionArcs(SdfPrimSpecHandle &primSpec) {

    ImGui::PushItemWidth(-1);
    if (ImGui::Button("Add reference")) {
        ImGui::OpenPopup("DrawPrimCompositionPopupMenu");
    }
    //ImVec2 boxSize(0, -10); // TODO : move box
    if (ImGui::ListBoxHeader("##compositionarcs")) {
        DrawPrimCompositions(primSpec);
        const auto &pathStr = primSpec->GetPath().GetString();
        SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
        TF_FOR_ALL(varSetIt, variantSetMap) {
            const SdfVariantSetSpecHandle &varSetSpec = varSetIt->second;
            const SdfVariantSpecHandleVector &variants = varSetSpec->GetVariantList();
            TF_FOR_ALL(varIt, variants) {
                const SdfPrimSpecHandle &variantSpec = (*varIt)->GetPrimSpec();
                DrawPrimCompositions(variantSpec);
            }
        }
        ImGui::ListBoxFooter();
    }

    if (ImGui::BeginPopupContextItem("DrawPrimCompositionPopupMenu")) {
        DrawPrimCompositionPopupMenu(primSpec);
        ImGui::EndPopup();
    }
}



void DrawPrimSpecAttributes(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;

    int deleteButtonCounter = 0;
    const auto &attributes = primSpec->GetAttributes();
    static ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##DrawPrimSpecAttributes", 1, tableFlags)) {
        ImGui::TableSetupColumn("Attribute");
        TF_FOR_ALL(attribute, attributes) {
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
            ImGui::TableNextRow();
            ImGui::PushID(deleteButtonCounter++);
            if(ImGui::Button("D")) { // This will be replaced by a "bin/trash" glyph
                ExecuteAfterDraw(&SdfPrimSpec::RemoveProperty, primSpec,
                                    primSpec->GetPropertyAtPath((*attribute)->GetPath()));
            }
            ImGui::PopID();
            ImGui::SameLine();
            if (ImGui::TreeNodeEx((*attribute)->GetName().c_str(), nodeFlags, "%s", (*attribute)->GetName().c_str())) {
                if ((*attribute)->HasDefaultValue()) {
                    ImGui::TableNextRow();
                    ImGui::PushID(deleteButtonCounter++);
                    if(ImGui::Button("D")) {
                        ExecuteAfterDraw(&SdfAttributeSpec::ClearDefaultValue, (*attribute));
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                    std::string defaultValueLabel = "Default";
                    VtValue modified = DrawVtValue(defaultValueLabel, (*attribute)->GetDefaultValue());
                    if (modified != VtValue()) {
                        ExecuteAfterDraw(&SdfAttributeSpec::SetDefaultValue, *attribute, modified);
                    }
                }

                SdfTimeSampleMap timeSamples = (*attribute)->GetTimeSampleMap();
                if (!timeSamples.empty()) {
                    TF_FOR_ALL(sample, timeSamples) {
                        ImGui::TableNextRow();
                        ImGui::PushID(deleteButtonCounter++);
                        if(ImGui::Button("D")) {
                            ExecuteAfterDraw(&SdfLayer::EraseTimeSample, primSpec->GetLayer(), (*attribute)->GetPath(), sample->first);
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                        std::string sampleValueLabel = std::to_string(sample->first);
                        VtValue modified = DrawVtValue(sampleValueLabel, sample->second);
                        if (modified!=VtValue()) {
                            ExecuteAfterDraw(&SdfLayer::SetTimeSample<VtValue>, primSpec->GetLayer(), (*attribute)->GetPath(), sample->first, modified);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }
}


void DrawPrimSpecEditor(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    ImGui::Text("%s", primSpec->GetLayer()->GetDisplayName().c_str());
    ImGui::Text("%s", primSpec->GetPath().GetString().c_str());
    if (!primSpec->GetPath().IsPrimVariantSelectionPath()) {
         if (ImGui::CollapsingHeader("Metadata")) {
            DrawPrimSpecifierCombo(primSpec);
            DrawPrimName(primSpec);
            DrawPrimKind(primSpec);
            DrawPrimType(primSpec);
            DrawPrimInstanceable(primSpec);
            DrawPrimHidden(primSpec);
            DrawPrimActive(primSpec);
        }
    }

    if (ImGui::CollapsingHeader("References")) {
        DrawPrimCompositionArcs(primSpec);
    }

    if (ImGui::CollapsingHeader("Attributes")) {
        DrawPrimSpecAttributes(primSpec);
    }
}
