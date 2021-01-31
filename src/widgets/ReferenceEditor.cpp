
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>

#include "ReferenceEditor.h"
#include "Gui.h"
#include "PrimSpecEditor.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "Commands.h"
#include "Constants.h"



static const char *ArcAppendChar = "Append";
static const char *ArcAddChar = "Add";
static const char *ArcPrependChar = "Prepend";
static const char *ArcDeleteChar = "Delete";
static const char *ArcExplicitChar = "Explicit";
static const char *ArcOrderedChar = "Ordered";

static const char* ReferenceCompositionT = "Reference";
static const char* PayloadCompositionT = "Payload";

// Utility to return the enumeration size
template <typename T> constexpr int enumSize();
template <> constexpr int enumSize<CompositionOperation>() { return 5; }
template <> constexpr int enumSize<CompositionList>() { return 4; }

template <typename IndexT> // int or ListOperation
const char *GetOperationName(IndexT index) {
    static const char *names[enumSize<CompositionOperation>()] = {"Add", "Prepend", "Append", "Remove", "Erase"};
    return names[static_cast<int>(index)];
}

template <typename IndexT> // int or CompositionList
const char *GetCompositionListName(IndexT index) {
    static const char *names[enumSize<CompositionList>()] = {"Reference", "Payload", "Inherit", "Specialize"};
    return names[static_cast<int>(index)];
}

template <typename CompositionT, typename ArgDefault, typename... ArgTypes>
void ApplyOperation(CompositionT arcList, CompositionOperation operation, ArgDefault arg, ArgTypes... others) {
    switch (operation) {
    case CompositionOperation::Add:
        arcList.Add({arg, others...});
        break;
    case CompositionOperation::Prepend:
        arcList.Prepend({arg, others...});
        break;
    case CompositionOperation::Append:
        arcList.Append({arg, others...});
        break;
    case CompositionOperation::Remove:
        arcList.Remove({arg, others...});
        break;
    case CompositionOperation::Erase:
        arcList.Erase({arg, others...});
        break;
    }
}

template <typename ArgDefault, typename... OtherArgTypes>
void ApplyOperationOnCompositionListTpl(SdfPrimSpecHandle &primSpec, CompositionOperation operation,
                                        CompositionList compositionList, ArgDefault arg, OtherArgTypes... others) {
    if (!primSpec)
        return;
    switch (compositionList) {
    case CompositionList::Reference:
        ApplyOperation(primSpec->GetReferenceList(), operation, arg, others...);
        break;
    case CompositionList::Payload:
        ApplyOperation(primSpec->GetPayloadList(), operation, arg, others...);
        break;
    case CompositionList::Specialize:
        ApplyOperation(primSpec->GetSpecializesList(), operation, SdfPath(arg));
        break;
    case CompositionList::Inherit:
        ApplyOperation(primSpec->GetInheritPathList(), operation, SdfPath(arg));
        break;
    }
}
void ApplyOperationOnCompositionList(SdfPrimSpecHandle &primSpec, CompositionOperation operation, CompositionList compositionList,
                                     const std::string &identifier, const SdfPath &targetPrim) {
    if (targetPrim.IsEmpty()) {
        ApplyOperationOnCompositionListTpl(primSpec, operation, compositionList, identifier);
    } else {
        ApplyOperationOnCompositionListTpl(primSpec, operation, compositionList, identifier, targetPrim);
    }
}

// TODO:
// There is a UsdReferences api that should probably be easier than the following code

// TODO: add all the composition arcs
struct AddReference : public ModalDialog {

    AddReference(SdfPrimSpecHandle &primSpec) : primSpec(primSpec){};

    ~AddReference() override {}

    void Draw() override {
        if (!primSpec) {
            CloseModal();
            return;
        }

        ImGui::Text("%s", primSpec->GetPath().GetString().c_str());

        if (fileBrowser) {
            if (ImGui::Button("\xef\x85\x9b")) {
                fileBrowser = !fileBrowser;
            }
            ImGui::SameLine();
            DrawFileBrowser();
            if (ImGui::Button("Use selected file")) {
                fileBrowser = !fileBrowser;
                referenceFilePath = GetFileBrowserFilePath();
            }
        } else {
            if (ImGui::BeginCombo("Operation", GetOperationName(operation))) {
                for (int n = 0; n < enumSize<CompositionOperation>(); n++) {
                    if (ImGui::Selectable(GetOperationName(n))) {
                        operation = CompositionOperation(n);
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("Composition type", GetCompositionListName(composition))) {
                for (int n = 0; n < enumSize<CompositionList>(); n++) {
                    if (ImGui::Selectable(GetCompositionListName(n))) {
                        composition = CompositionList(n);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("File path", &referenceFilePath);
            ImGui::SameLine();
            if (ImGui::Button("\xef\x85\x9b")) {
                fileBrowser = !fileBrowser;
            }
            ImGui::InputText("Target prim path", &targetPrimPath);
            DrawOkCancelModal([=]() {
                ExecuteAfterDraw<PrimCreateCompositionArc>(primSpec, operation, composition, referenceFilePath, SdfPath(targetPrimPath));
            });
        }
    }

    const char *DialogId() const override { return "Add reference"; }
    SdfPrimSpecHandle primSpec;
    CompositionOperation operation = CompositionOperation::Add;
    CompositionList composition = CompositionList::Reference;
    std::string referenceFilePath;
    std::string  targetPrimPath;

    bool fileBrowser = false;
};

static inline const char *GetPathFrom(const SdfPath &value) { return value.GetString().c_str(); }
static inline const char *GetPathFrom(const SdfReference &value) { return value.GetAssetPath().c_str(); }
static inline const char *GetPathFrom(const SdfPayload &value) { return value.GetAssetPath().c_str(); }

static inline const char *GetTargetNameFrom(const SdfPath &value) { return value.GetString().c_str(); }
static inline const char *GetTargetNameFrom(const SdfReference &value) { return value.GetPrimPath().GetString().c_str(); }
static inline const char *GetTargetNameFrom(const SdfPayload &value) { return value.GetPrimPath().GetString().c_str(); }

template <typename T>
static inline std::string GetDisplayNameFrom(const T &value) {
    std::stringstream ss;
    ss << value;
    return ss.str();
}


void DrawPrimAddReferenceModalDialog(SdfPrimSpecHandle& primSpec) {
    DrawModalDialog<AddReference>(primSpec);
}


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


// TODO: simplify the following function
template <typename GetListFuncT, typename GetItemsFuncT>
void DrawReferencePopupContextItem(const SdfPrimSpecHandle &primSpec, const SdfPath &path, GetListFuncT listFunc,
                                   GetItemsFuncT itemsFunc, int index, const char *pathFromRef) {
    if (ImGui::BeginPopupContextItem(pathFromRef)) {
        // TODO: simplify, or create a specific command
        if (ImGui::MenuItem("Remove reference")) {
            std::function<void()> deferedRemoveArc = [=]() {
                PrimSpecRemoveArc(primSpec->GetLayer(), path, listFunc, itemsFunc, index);
            };
            ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), deferedRemoveArc);
        }
        if (ImGui::MenuItem("Open as Stage")) {
            ExecuteAfterDraw<EditorOpenStage>(std::string(pathFromRef));
        }
        if (ImGui::MenuItem("Edit")) {
            ExecuteAfterDraw<EditorOpenLayer>(std::string(pathFromRef));
        }
        if (ImGui::MenuItem("Copy path")) {
            ImGui::SetClipboardText(pathFromRef);
        }
        ImGui::EndPopup();
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
        //auto variantSelection = path.GetVariantSelection();
        //ImGui::Text("%s %s", variantSelection.first.c_str(), variantSelection.second.c_str());
        //ImGui::SameLine();
        ImGui::Text(ICON_FA_ARROW_RIGHT " %s %s" , operation, compositionType);
        ImGui::SameLine();
        ImGui::Text("%s %s", GetDisplayNameFrom(ref).c_str(), GetTargetNameFrom(ref));
        DrawReferencePopupContextItem(primSpec, path, listFunc, itemsFunc, index, GetPathFrom(ref));
        index++;
    }
}

template <typename GetListFuncT, typename GetItemsFuncT>
void DrawCompositionArcItemsRow(const SdfPrimSpecHandle &primSpec, const char *operation,
                             const char *compositionType, GetListFuncT listFunc, GetItemsFuncT itemsFunc) {
    auto path = primSpec->GetPath();
    auto arcList = std::bind(listFunc, get_pointer(primSpec))();
    auto items = std::bind(itemsFunc,  &arcList)();
    size_t index = 0;
    for (const auto &ref : items) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s %s", operation, compositionType);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(GetPathFrom(ref));
        DrawReferencePopupContextItem(primSpec, path, listFunc, itemsFunc, index, GetPathFrom(ref));
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

template <typename GetListFuncT>
void DrawCompositionArcOperationsRows(const SdfPrimSpecHandle &primSpec, GetListFuncT listFunc,
                                  const char *compositionType) {
    using SdfListT = decltype(std::bind(listFunc, get_pointer(primSpec))());
    DrawCompositionArcItemsRow(primSpec, ArcAppendChar, compositionType, listFunc, &SdfListT::GetAppendedItems);
    DrawCompositionArcItemsRow(primSpec, ArcAddChar, compositionType, listFunc, &SdfListT::GetAddedItems);
    DrawCompositionArcItemsRow(primSpec, ArcPrependChar, compositionType, listFunc, &SdfListT::GetPrependedItems);
    DrawCompositionArcItemsRow(primSpec, ArcDeleteChar, compositionType, listFunc, &SdfListT::GetDeletedItems);
    DrawCompositionArcItemsRow(primSpec, ArcExplicitChar, compositionType, listFunc, &SdfListT::GetExplicitItems);
    DrawCompositionArcItemsRow(primSpec, ArcOrderedChar, compositionType, listFunc, &SdfListT::GetOrderedItems);
}

/// Draw all compositions in one big list
void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec) {
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetPayloadList, "Payload");
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetInheritPathList, "Inherit");
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetSpecializesList, "Specialize");
    DrawCompositionArcOperations(primSpec, &SdfPrimSpec::GetReferenceList, "Reference");
}

void DrawPrimCompositionsRows(const SdfPrimSpecHandle &primSpec) {
    DrawCompositionArcOperationsRows(primSpec, &SdfPrimSpec::GetPayloadList, "Payload");
    DrawCompositionArcOperationsRows(primSpec, &SdfPrimSpec::GetInheritPathList, "Inherit");
    DrawCompositionArcOperationsRows(primSpec, &SdfPrimSpec::GetSpecializesList, "Specialize");
    DrawCompositionArcOperationsRows(primSpec, &SdfPrimSpec::GetReferenceList, "Reference");
}

void DrawPrimCompositionSummary(SdfPrimSpecHandle &primSpec, bool showVariants) {
    if (PrimHasComposition(primSpec)) {
        DrawPrimCompositions(primSpec);
        if (showVariants) {
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


void DrawPrimCompositionArcs(SdfPrimSpecHandle &primSpec, bool showVariant) {

    if (ImGui::BeginTable("##DrawVariantSetsCombos", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("Compositions");
        ImGui::TableSetupColumn("Identifier");
        ImGui::TableSetupColumn("Target");
        ImGui::TableHeadersRow();

        DrawPrimCompositionsRows(primSpec);
        if (showVariant) {
            const auto& pathStr = primSpec->GetPath().GetString();
            SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
            TF_FOR_ALL(varSetIt, variantSetMap) {
                const SdfVariantSetSpecHandle& varSetSpec = varSetIt->second;
                const SdfVariantSpecHandleVector& variants = varSetSpec->GetVariantList();
                TF_FOR_ALL(varIt, variants) {
                    const SdfPrimSpecHandle& variantSpec = (*varIt)->GetPrimSpec();
                    DrawPrimCompositionsRows(variantSpec);
                }
            }
        }
        ImGui::EndTable();
    }
}



