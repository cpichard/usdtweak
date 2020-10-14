
#include <iostream>
#include <array>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/schema.h>
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
            DispatchCommand<PrimAddReference>(primSpec, reference);
            CloseModal();
        }
    }

    const char *DialogId() const override { return "Edit Prim References"; }
    SdfPrimSpecHandle primSpec;
    int operation;
    int composition;
};

void DrawPrimSpecifierCombo(SdfPrimSpecHandle &primSpec) {

    // TODO: is this list already available in USD ?
    static SdfSpecifier specList[] = {SdfSpecifierDef, SdfSpecifierOver, SdfSpecifierClass};

    const SdfSpecifier current = primSpec->GetSpecifier();
    SdfSpecifier selected = current;
    const std::string specifierName = TfEnum::GetDisplayName(current);
    if (ImGui::BeginCombo("Prim Specifier", specifierName.c_str())) {
        for (int n = 0; n < IM_ARRAYSIZE(specList); n++) {
            const SdfSpecifier displayed = specList[n];
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
            DispatchCommand<PrimChangeName>(primSpec, std::move(primName));
        }
    }
}

/// Draw a prim type name combo
void DrawPrimType(SdfPrimSpecHandle &primSpec) {

    /// TODO reset to none as well
    /// TODO: look at: https://github.com/ocornut/imgui/issues/282
    // TODO: get all types, ATM the function GetAllTypes is not resolved at link time
    // auto allTypes = primSpec->GetSchema().GetAllTypes();
    // They are also registered in "registry.usda"
    const char *classes[] = {"Scope",      "Xform",        "Cube",     "Sphere",         "Cylinder",
                             "Capsule",    "Cone",         "Camera",   "PointInstancer", "Mesh",
                             "GeomSubset", "DistantLight", "Material", "Shader", "BlendShape"};
    const char *currentItem = primSpec->GetTypeName().GetString().c_str();

    if (ImGui::BeginCombo("Prim Type", currentItem)) {
        for (int n = 0; n < IM_ARRAYSIZE(classes); n++) {
            bool isSelected = strcmp(currentItem, classes[n]) == 0;
            if (ImGui::Selectable(classes[n], isSelected)) {
                currentItem = classes[n];
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }

        if (currentItem && primSpec->GetTypeName() != currentItem) {
            ExecuteAfterDraw(&SdfPrimSpec::SetTypeName, primSpec, currentItem);
        }
        ImGui::EndCombo();
    }
}

static inline const char *GetPathFrom(const SdfPath &value) { return value.GetString().c_str(); }
static inline const char *GetPathFrom(const SdfReference &value) { return value.GetAssetPath().c_str(); }
static inline const char *GetPathFrom(const SdfPayload &value) { return value.GetAssetPath().c_str(); }

template <typename ArcT>
void DrawCompositionArcItems(const SdfPath &path, const char *operation,
                             const std::vector<typename ArcT::value_type> &items,
                             const char *compositionType) {
    for (const auto &ref : items) {
        auto variantSelection = path.GetVariantSelection();
        ImGui::Text("%s %s", variantSelection.first.c_str(), variantSelection.second.c_str());
        ImGui::SameLine();
        ImGui::Text("%s %s", operation, compositionType);
        ImGui::SameLine();
        ImGui::TextUnformatted(GetPathFrom(ref));
        // TODO popup menu with JumpTo, Delete, Replace, etc
    }
}

template <typename ArcT>
void DrawCompositionArcOperations(const SdfPath &path, SdfListEditorProxy<ArcT> &&refList,
                                  const char *compositionType) {
    DrawCompositionArcItems<ArcT>(path, ArcAppendChar, refList.GetAppendedItems(), compositionType);
    DrawCompositionArcItems<ArcT>(path, ArcAddChar, refList.GetAddedItems(), compositionType);
    DrawCompositionArcItems<ArcT>(path, ArcPrependChar, refList.GetPrependedItems(), compositionType);
    DrawCompositionArcItems<ArcT>(path, ArcDeleteChar, refList.GetDeletedItems(), compositionType);
    DrawCompositionArcItems<ArcT>(path, ArcExplicitChar, refList.GetExplicitItems(), compositionType);
    DrawCompositionArcItems<ArcT>(path, ArcOrderedChar, refList.GetOrderedItems(), compositionType);
}

/// Draw all compositions in one big list
void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec) {
    DrawCompositionArcOperations(primSpec->GetPath(), primSpec->GetPayloadList(), "Payload");
    DrawCompositionArcOperations(primSpec->GetPath(), primSpec->GetInheritPathList(), "Inherit");
    DrawCompositionArcOperations(primSpec->GetPath(), primSpec->GetSpecializesList(), "Specialize");
    DrawCompositionArcOperations(primSpec->GetPath(), primSpec->GetReferenceList(), "Reference");
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
bool PrimHasComposition(SdfPrimSpecHandle &primSpec, bool checkVariants) {

    if (HasComposition(primSpec)){
        return true;
    }

    if (checkVariants) {
        SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
        TF_FOR_ALL(varSetIt, variantSetMap) {
            const SdfVariantSetSpecHandle &varSetSpec = varSetIt->second;
            const SdfVariantSpecHandleVector &variants = varSetSpec->GetVariantList();
            TF_FOR_ALL(varIt, variants) {
                if (HasComposition((*varIt)->GetPrimSpec())){
                    return true;
                }
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
            TriggerOpenModal<EditReferences>(primSpec);
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
    ImVec2 boxSize(0, -50); // TODO : move box
    if (ImGui::ListBoxHeader("##compositionarcs", boxSize)) {
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
    if (ImGui::Button("Add composition")) {
        ImGui::OpenPopup("DrawPrimCompositionPopupMenu");
    }
    ImGui::SameLine();
    ImGui::Button("Remove selected");
    if (ImGui::BeginPopupContextItem("DrawPrimCompositionPopupMenu")) {
        DrawPrimCompositionPopupMenu(primSpec);
        ImGui::EndPopup();
    }
}

void DrawPrimSpecAttributes(SdfPrimSpecHandle &primSpec) {
    if (!primSpec)
        return;
    const auto &attributes = primSpec->GetAttributes();
    static ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##DrawPrimSpecAttributes", 1, tableFlags)) {
        ImGui::TableSetupColumn("Attribute");
        TF_FOR_ALL(attribute, attributes) {
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
            ImGui::TableNextRow();
            if (ImGui::TreeNodeEx((*attribute)->GetName().c_str(), nodeFlags, "%s", (*attribute)->GetName().c_str())) {
                if ((*attribute)->HasDefaultValue()) {
                    ImGui::TableNextRow();
                    std::string defaultValueLabel = (*attribute)->GetName() + " Default";
                    DrawVtValue(defaultValueLabel, (*attribute)->GetDefaultValue());
                }

                SdfTimeSampleMap timeSamples = (*attribute)->GetTimeSampleMap();
                if (!timeSamples.empty()) {
                    TF_FOR_ALL(sample, timeSamples) {
                        ImGui::TableNextRow();
                        std::string sampletValueLabel = (*attribute)->GetName() + " " + std::to_string(sample->first);
                        DrawVtValue(sampletValueLabel, sample->second);
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
    ImGui::Text("%s", primSpec->GetPath().GetString().c_str());
    DrawPrimSpecifierCombo(primSpec);
    DrawPrimName(primSpec);
    //// Kind: component/assembly, etc add a combo
    ////ImGui::Text("%s", primSpec->GetKind().GetString().c_str());
    DrawPrimType(primSpec);
    DrawPrimInstanceable(primSpec);
    DrawPrimHidden(primSpec);
    DrawPrimActive(primSpec);
    ImGui::BeginTabBar("primspeceditortabbar");
    if (ImGui::BeginTabItem("Attributes")) {
        DrawPrimSpecAttributes(primSpec);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Compositions")) {
        DrawPrimCompositionArcs(primSpec);
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}
