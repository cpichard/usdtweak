
#include "Commands.h"
#include "CompositionEditor.h"
#include "Constants.h"
#include "FileBrowser.h"
#include "ImGuiHelpers.h"
#include "ModalDialogs.h"
#include "UsdHelpers.h"
#include "ImGuiHelpers.h"
#include "EditListSelector.h"
#include "TableLayouts.h"

#include <algorithm>
#include <iostream>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>

typedef enum CompositionArcListType { ReferenceList, PayloadList, InheritList, SpecializeList } CompositionArcListType;

// Unfortunately Inherit and Specialize are just alias to SdfPath, so there is no way to differentiate the
// edit list from the type.
// To reuse templated code we create new type SdfInherit and SdfSpecialize them
template <int InheritOrSpecialize> struct SdfInheritOrSpecialize : SdfPath {
    SdfInheritOrSpecialize() : SdfPath() {}
    SdfInheritOrSpecialize(const SdfPath &path) : SdfPath(path) {}
};

using SdfInherit = SdfInheritOrSpecialize<0>;
using SdfSpecialize = SdfInheritOrSpecialize<1>;

template <typename ArcListItemT> struct ArcListItemTrait { typedef ArcListItemT type; };
template <> struct ArcListItemTrait<SdfInherit> { typedef SdfPath type; };
template <> struct ArcListItemTrait<SdfSpecialize> { typedef SdfPath type; };

static bool HasComposition(const SdfPrimSpecHandle &primSpec) {
    return primSpec->HasReferences() || primSpec->HasPayloads() || primSpec->HasInheritPaths() || primSpec->HasSpecializes();
}

inline SdfReferencesProxy GetCompositionArcList(const SdfPrimSpecHandle &primSpec, const SdfReference &val) {
    return primSpec->GetReferenceList();
}

inline SdfPayloadsProxy GetCompositionArcList(const SdfPrimSpecHandle &primSpec, const SdfPayload &val) {
    return primSpec->GetPayloadList();
}

inline SdfInheritsProxy GetCompositionArcList(const SdfPrimSpecHandle &primSpec, const SdfInherit &val) {
    return primSpec->GetInheritPathList();
}

inline SdfSpecializesProxy GetCompositionArcList(const SdfPrimSpecHandle &primSpec, const SdfSpecialize &val) {
    return primSpec->GetSpecializesList();
}


inline void InspectArcType(const SdfPrimSpecHandle &primSpec, const SdfReference &ref) {
    auto realPath = primSpec->GetLayer()->ComputeAbsolutePath(ref.GetAssetPath());
    auto layerOrOpen = SdfLayer::FindOrOpen(realPath);
    ExecuteAfterDraw<EditorInspectLayerLocation>(layerOrOpen, ref.GetPrimPath());
}

inline void InspectArcType(const SdfPrimSpecHandle &primSpec, const SdfPayload &pay) {
    auto realPath = primSpec->GetLayer()->ComputeAbsolutePath(pay.GetAssetPath());
    auto layerOrOpen = SdfLayer::FindOrOpen(realPath);
    ExecuteAfterDraw<EditorInspectLayerLocation>(layerOrOpen, pay.GetPrimPath());
}

inline void InspectArcType(const SdfPrimSpecHandle &primSpec, const SdfPath &path) {
   ExecuteAfterDraw<EditorInspectLayerLocation>(primSpec->GetLayer(), path);
}

/// Create a standard UI for entering a SdfPath.
/// This is used for inherit and specialize
struct CreateSdfPathModalDialog : public ModalDialog {

    CreateSdfPathModalDialog(const SdfPrimSpecHandle &primSpec) : _primSpec(primSpec){};
    ~CreateSdfPathModalDialog() override {}

    void Draw() override {
        if (!_primSpec) {
            CloseModal();
            return;
        }
        // TODO: We will probably want to browse in the scene hierarchy to select the path
        //   create a selection tree, one day
        ImGui::Text("%s", _primSpec->GetPath().GetString().c_str());
        if (ImGui::BeginCombo("Operation", GetListEditorOperationName(_operation))) {
            for (int n = 0; n < GetListEditorOperationSize(); n++) {
                if (ImGui::Selectable(GetListEditorOperationName(SdfListOpType(n)))) {
                    _operation = SdfListOpType(n);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::InputText("Target prim path", &_primPath);
        DrawOkCancelModal([=]() { OnOkCallBack(); });
    }

    virtual void OnOkCallBack() = 0;

    const char *DialogId() const override { return "Sdf path"; }

    SdfPrimSpecHandle _primSpec;
    std::string _primPath;
    SdfListOpType _operation = SdfListOpTypeExplicit;
};

/// UI used to create an AssetPath having a file path to a layer and a
/// target prim path.
/// This is used by References and Payloads which share the same interface
struct CreateAssetPathModalDialog : public ModalDialog {

    CreateAssetPathModalDialog(const SdfPrimSpecHandle &primSpec) : _primSpec(primSpec){};
    ~CreateAssetPathModalDialog() override {}

    void Draw() override {
        if (!_primSpec) {
            CloseModal();
            return;
        }
        ImGui::Text("%s", _primSpec->GetPath().GetString().c_str());

        if (ImGui::BeginCombo("Operation", GetListEditorOperationName(_operation))) {
            for (int n = 0; n < GetListEditorOperationSize(); n++) {
                if (ImGui::Selectable(GetListEditorOperationName(n))) {
                    _operation = static_cast<SdfListOpType>(n);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::InputText("File path", &_assetPath);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE)) {
            ImGui::OpenPopup("Asset path browser");
        }
        if (ImGui::BeginPopupModal("Asset path browser")) {
            DrawFileBrowser();
            ImGui::Checkbox("Use relative path", &_relative);
            ImGui::Checkbox("Unix compatible", &_unixify);
            if (ImGui::Button("Use selected file")) {
                if (_relative) {
                    _assetPath = GetFileBrowserFilePathRelativeTo(_primSpec->GetLayer()->GetRealPath(), _unixify);
                } else {
                    _assetPath = GetFileBrowserFilePath();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::InputText("Target prim path", &_primPath);
        ImGui::InputDouble("Layer time offset", &_timeOffset);
        ImGui::InputDouble("Layer time scale", &_timeScale);
        DrawOkCancelModal([=]() { OnOkCallBack(); });
    }

    virtual void OnOkCallBack() = 0;

    const char *DialogId() const override { return "Asset path"; }

    SdfLayerOffset GetLayerOffset() const {
        return (_timeScale != 1.0 || _timeOffset != 0.0) ? SdfLayerOffset(_timeOffset, _timeScale) : SdfLayerOffset();
    }

    SdfPrimSpecHandle _primSpec;
    std::string _assetPath;
    std::string _primPath;
    SdfListOpType _operation = SdfListOpTypeExplicit;

    bool _relative = false;
    bool _unixify = false;
    double _timeScale = 1.0;
    double _timeOffset = 0.0;
};

// Inheriting, but could also be done with templates, would the code be cleaner ?
struct CreateReferenceModalDialog : public CreateAssetPathModalDialog {
    CreateReferenceModalDialog(const SdfPrimSpecHandle &primSpec) : CreateAssetPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create reference"; }
    void OnOkCallBack() override {
        SdfReference reference(_assetPath, SdfPath(_primPath), GetLayerOffset());
        ExecuteAfterDraw<PrimCreateReference>(_primSpec, _operation, reference);
    }
};

struct CreatePayloadModalDialog : public CreateAssetPathModalDialog {
    CreatePayloadModalDialog(const SdfPrimSpecHandle &primSpec) : CreateAssetPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create payload"; }
    void OnOkCallBack() override {
        SdfPayload payload(_assetPath, SdfPath(_primPath), GetLayerOffset());
        ExecuteAfterDraw<PrimCreatePayload>(_primSpec, _operation, payload);
    }
};

struct CreateInheritModalDialog : public CreateSdfPathModalDialog {
    CreateInheritModalDialog(const SdfPrimSpecHandle &primSpec) : CreateSdfPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create inherit"; }
    void OnOkCallBack() override { ExecuteAfterDraw<PrimCreateInherit>(_primSpec, _operation, SdfPath(_primPath)); }
};

struct CreateSpecializeModalDialog : public CreateSdfPathModalDialog {
    CreateSpecializeModalDialog(const SdfPrimSpecHandle &primSpec) : CreateSdfPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create specialize"; }
    void OnOkCallBack() override { ExecuteAfterDraw<PrimCreateSpecialize>(_primSpec, _operation, SdfPath(_primPath)); }
};

void DrawPrimCreateReference(const SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreateReferenceModalDialog>(primSpec); }
void DrawPrimCreatePayload(const SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreatePayloadModalDialog>(primSpec); }
void DrawPrimCreateInherit(const SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreateInheritModalDialog>(primSpec); }
void DrawPrimCreateSpecialize(const SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreateSpecializeModalDialog>(primSpec); }

static SdfListOpType opList = SdfListOpTypeExplicit;

template <typename ArcT> void DrawArcCreationDialog(const SdfPrimSpecHandle &primSpec, SdfListOpType opList);
template <> void DrawArcCreationDialog<SdfReference>(const SdfPrimSpecHandle &primSpec, SdfListOpType opList) {
    DrawModalDialog<CreateReferenceModalDialog>(primSpec);
}
template <> void DrawArcCreationDialog<SdfPayload>(const SdfPrimSpecHandle &primSpec, SdfListOpType opList) {
    DrawModalDialog<CreatePayloadModalDialog>(primSpec);
}
template <> void DrawArcCreationDialog<SdfInherit>(const SdfPrimSpecHandle &primSpec, SdfListOpType opList) {
    DrawModalDialog<CreateInheritModalDialog>(primSpec);
}

template <> void DrawArcCreationDialog<SdfSpecialize>(const SdfPrimSpecHandle &primSpec, SdfListOpType opList) {
    DrawModalDialog<CreateSpecializeModalDialog>(primSpec);
}
template<typename ArcT>
inline void RemoveArc(const SdfPrimSpecHandle &primSpec, const ArcT &arc) {
    std::function<void()> removeItem = [=]() { GetCompositionArcList(primSpec, arc).RemoveItemEdits(arc); };
    ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removeItem);
}

///
template <typename ArcT> void DrawSdfPathMenuItems(const SdfPrimSpecHandle &primSpec, const SdfPath &path) {
    if (ImGui::MenuItem("Remove")) {
        if (!primSpec)
            return;
        RemoveArc(primSpec, ArcT(path));
    }
    if (ImGui::MenuItem("Copy path")) {
        ImGui::SetClipboardText(path.GetString().c_str());
    }
}

/// Draw the menu items for AssetPaths (SdfReference and SdfPayload)
template <typename AssetPathT> void DrawAssetPathMenuItems(const SdfPrimSpecHandle &primSpec, const AssetPathT &assetPath) {

    if (ImGui::MenuItem("Select Arc")) {
        auto realPath = primSpec->GetLayer()->ComputeAbsolutePath(assetPath.GetAssetPath());
        ExecuteAfterDraw<EditorFindOrOpenLayer>(realPath);
    }
    if (ImGui::MenuItem("Open as Stage")) {
        auto realPath = primSpec->GetLayer()->ComputeAbsolutePath(assetPath.GetAssetPath());
        ExecuteAfterDraw<EditorOpenStage>(realPath);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Remove")) {
        if (!primSpec)
            return;
        RemoveArc(primSpec, assetPath);
     }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy asset path")) {
        ImGui::SetClipboardText(assetPath.GetAssetPath().c_str());
    }
}

template <typename InheritOrSpecialize>
InheritOrSpecialize DrawSdfPathEditor(const SdfPrimSpecHandle &primSpec, const InheritOrSpecialize &arc, ImVec2 outerSize) {
    InheritOrSpecialize updatedArc;
    std::string updatedPath = arc.GetString();
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PreciseWidths |
                                           ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("DrawSdfPathEditor", 1, tableFlags, outerSize)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##assetpath", &updatedPath);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            updatedArc = SdfPath(updatedPath);
        }
        ImGui::EndTable();
    }
    return updatedArc;
}

// Works with SdfReference and SdfPayload
template <typename ReferenceOrPayloadT>
ReferenceOrPayloadT DrawReferenceOrPayloadEditor(const SdfPrimSpecHandle &primSpec, const ReferenceOrPayloadT &ref,
                                                 ImVec2 outerSize) {
    ReferenceOrPayloadT ret;
    std::string updatedPath = ref.GetAssetPath();
    std::string targetPath = ref.GetPrimPath().GetString();
    SdfLayerOffset layerOffset = ref.GetLayerOffset();
    float offset = layerOffset.GetOffset();
    float scale = layerOffset.GetScale();
    ImGui::PushID("DrawAssetPathArcEditor");
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PreciseWidths |
                                           ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("DrawAssetPathArcEditorTable", 4, tableFlags, outerSize)) {
        const float stretchLayer = (layerOffset == SdfLayerOffset()) ? 0.01 : 0.1;
        const float stretchTarget = (targetPath.empty()) ? 0.01 : 0.3 * (1 - 2 * stretchLayer);
        const float stretchPath = 1 - 2 * stretchLayer - stretchTarget;
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch, stretchPath);
        ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch, stretchTarget);
        ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthStretch, stretchLayer);
        ImGui::TableSetupColumn("scale", ImGuiTableColumnFlags_WidthStretch, stretchLayer);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##assetpath", &updatedPath);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ret = ref;
            ret.SetAssetPath(updatedPath);
        }
        // TODO more operation on the path: unixify, make relative, etc
        if (ImGui::BeginPopupContextItem("sublayer")) {
            ImGui::Text("%s", updatedPath.c_str());
            DrawAssetPathMenuItems(primSpec, ref);
            ImGui::EndPopup();
        }
        ImGui::SameLine();

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##targetpath", &targetPath);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ret = ref;
            ret.SetPrimPath(SdfPath(targetPath));
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat("##offset", &offset);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            SdfLayerOffset updatedLayerOffset(layerOffset);
            updatedLayerOffset.SetOffset(offset);
            ret = ref;
            ret.SetLayerOffset(updatedLayerOffset);
        }

        ImGui::TableSetColumnIndex(3);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat("##scale", &scale);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            SdfLayerOffset updatedLayerOffset(layerOffset);
            updatedLayerOffset.SetScale(scale);
            ret = ref;
            ret.SetLayerOffset(updatedLayerOffset);
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return ret;
}

inline SdfReference DrawCompositionArcEditor(const SdfPrimSpecHandle &primSpec, const SdfReference &arc, ImVec2 outerSize) {
    return DrawReferenceOrPayloadEditor(primSpec, arc, outerSize);
}

inline SdfPayload DrawCompositionArcEditor(const SdfPrimSpecHandle &primSpec, const SdfPayload &arc, ImVec2 outerSize) {
    return DrawReferenceOrPayloadEditor(primSpec, arc, outerSize);
}

inline SdfInherit DrawCompositionArcEditor(const SdfPrimSpecHandle &primSpec, const SdfInherit &arc, ImVec2 outerSize) {
    return DrawSdfPathEditor(primSpec, arc, outerSize);
}

inline SdfSpecialize DrawCompositionArcEditor(const SdfPrimSpecHandle &primSpec, const SdfSpecialize &arc, ImVec2 outerSize) {
    return DrawSdfPathEditor(primSpec, arc, outerSize);
}

template <typename CompositionArcT>
void DrawCompositionArcRow(int rowId, const SdfPrimSpecHandle &primSpec, const CompositionArcT &arc, const SdfListOpType &op) {
    // Arbitrary outer size for the reference editor
    using ItemType = typename ArcListItemTrait<CompositionArcT>::type;
    const float regionAvailable = ImGui::GetWindowWidth() - 3 * 28 - 20; // 28 to account for buttons, 20 is arbitrary
    ImVec2 outerSize(regionAvailable, TableRowDefaultHeight);
    CompositionArcT updatedArc = DrawCompositionArcEditor(primSpec, arc, outerSize);
    if (updatedArc != CompositionArcT()) {
        std::function<void()> updateReferenceFun = [=]() {
            auto arcList = GetCompositionArcList(primSpec, arc);
            auto editList = GetSdfListOp(arcList, op);
            editList[rowId] = updatedArc;
        };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), updateReferenceFun);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_UP)) {
        std::function<void()> moveUp = [=]() {
            auto arcList = GetCompositionArcList(primSpec, arc);
            auto editList = GetSdfListOp(arcList, op);
            if (rowId >= 1) { // std::swap doesn't compile here
                auto it = editList[rowId];
                ItemType tmp = it;
                editList.erase(editList.begin() + rowId);
                editList.insert(editList.begin() + rowId - 1, tmp);
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), moveUp);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_DOWN)) {
        std::function<void()> moveDown = [=]() {
            auto arcList = GetCompositionArcList(primSpec, arc);
            auto editList = GetSdfListOp(arcList, op);
            if (rowId < editList.size() - 1) { // std::swap doesn't compile here
                auto it = editList[rowId];
                ItemType tmp = it;
                editList.erase(editList.begin() + rowId);
                editList.insert(editList.begin() + rowId + 1, tmp);
            }
        };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), moveDown);
    }
}

struct CompositionArcRow {}; // Rany of SdfReference SdfPayload SdfInherit and SdfSpecialize

#define GENERATE_ARC_DRAW_CODE(ClassName_)                                                                                  \
template <>                                                                                                                  \
inline void DrawFirstColumn<CompositionArcRow>(int rowId, const SdfPrimSpecHandle &primSpec, const ClassName_ &arc,          \
                                               const SdfListOpType &chosenList) {                                            \
    if (ImGui::Button(ICON_FA_TRASH)) {                                                                                      \
        RemoveArc(primSpec, arc);                                                                                            \
    }                                                                                                                        \
}                                                                                                                            \
                                                                                                                             \
template <>                                                                                                                  \
inline void DrawSecondColumn<CompositionArcRow>(int rowId, const SdfPrimSpecHandle &primSpec, const ClassName_ &arc,         \
                                                const SdfListOpType &chosenList) {                                           \
    DrawCompositionArcRow(rowId, primSpec, arc, chosenList);                                                                 \
}
GENERATE_ARC_DRAW_CODE(SdfReference)
GENERATE_ARC_DRAW_CODE(SdfPayload)
GENERATE_ARC_DRAW_CODE(SdfInherit)
GENERATE_ARC_DRAW_CODE(SdfSpecialize)

template <typename CompositionArcItemT> void DrawCompositionEditor(const SdfPrimSpecHandle &primSpec) {
    using ItemType = typename ArcListItemTrait<CompositionArcItemT>::type;

    if (ImGui::Button(ICON_FA_PLUS)) {
        DrawArcCreationDialog<CompositionArcItemT>(primSpec, opList);
    }
   
    auto arcList = GetCompositionArcList(primSpec, CompositionArcItemT());
    SdfListOpType opList = GetEditListChoice(arcList);

    ImGui::SameLine();
    DrawEditListComboSelector(opList, arcList);

    if (BeginTwoColumnsTable("##AssetTypeTable")) {
        auto editList = GetSdfListOp(arcList, opList);
        for (int ind = 0; ind < editList.size(); ind++) {
            const ItemType &item = editList[ind];
            ImGui::PushID(boost::hash<ItemType>{}(item));
            const CompositionArcItemT arcItem(item); // Sadly we have to copy here,
            DrawTwoColumnsRow<CompositionArcRow>(ind, primSpec, arcItem, opList);
            ImGui::PopID();
        }
        EndTwoColumnsTable();
    }
}

void DrawPrimCompositions(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec || !HasComposition(primSpec))
        return;
    if (ImGui::CollapsingHeader("Composition", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTabBar("##CompositionType")) {
            if (ImGui::BeginTabItem("References", nullptr,
                                    primSpec->HasReferences() ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None)) {
                DrawCompositionEditor<SdfReference>(primSpec);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Payloads", nullptr,
                                    primSpec->HasPayloads() ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None)) {
                DrawCompositionEditor<SdfPayload>(primSpec);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Inherits", nullptr,
                                    primSpec->HasInheritPaths() ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None)) {
                DrawCompositionEditor<SdfInherit>(primSpec);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Specializes", nullptr,
                                    primSpec->HasSpecializes() ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None)) {
                DrawCompositionEditor<SdfSpecialize>(primSpec);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
}
/////////////// Summaries used in the layer scene editor

template <typename ArcT>
inline void DrawSdfPathSummary(std::string &&header, const char *operation, const SdfPath &path,
                               const SdfPrimSpecHandle &primSpec, int &menuItemId) {
    ScopedStyleColor transparentStyle(ImGuiCol_Button, ImVec4(ColorTransparent));
    ImGui::PushID(menuItemId++);
    if (ImGui::Button(ICON_FA_TRASH)) {
        RemoveArc(primSpec, ArcT(path));
    }
    ImGui::SameLine();
    std::string summary = path.GetString();
    if (ImGui::SmallButton(summary.c_str())) {
        InspectArcType(primSpec, path);
    }
    if (ImGui::BeginPopupContextItem()) {
        DrawSdfPathMenuItems<ArcT>(primSpec, path);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

template <typename AssetPathT>
inline void DrawAssetPathSummary(std::string &&header, const char *operation, const AssetPathT &assetPath,
                                 const SdfPrimSpecHandle &primSpec, int &menuItemId) {
    ScopedStyleColor transparentStyle(ImGuiCol_Button, ImVec4(ColorTransparent));
    ImGui::PushID(menuItemId++);
    if (ImGui::Button(ICON_FA_TRASH)) {
        RemoveArc(primSpec, assetPath);
    }
    ImGui::PopID();
    ImGui::SameLine();
    std::string summary(operation);
    summary += " ";
    summary += assetPath.GetAssetPath().empty() ? "" : "@" + assetPath.GetAssetPath() + "@";
    summary += assetPath.GetPrimPath().GetString().empty() ? "" : "<" + assetPath.GetPrimPath().GetString() + ">";
    ImGui::PushID(menuItemId++);
    if(ImGui::Button(summary.c_str())) {
        InspectArcType(primSpec, assetPath);
    }
    if (ImGui::BeginPopupContextItem("###AssetPathMenuItems")) {
        DrawAssetPathMenuItems(primSpec, assetPath);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void DrawReferenceSummary(const char *operation, const SdfReference &assetPath, const SdfPrimSpecHandle &primSpec,
                          int &menuItemId) {
    DrawAssetPathSummary("References", operation, assetPath, primSpec, menuItemId);
}

void DrawPayloadSummary(const char *operation, const SdfPayload &assetPath, const SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawAssetPathSummary("Payloads", operation, assetPath, primSpec, menuItemId);
}

void DrawInheritPathSummary(const char *operation, const SdfPath &path, const SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawSdfPathSummary<SdfInherit>("Inherits", operation, path, primSpec, menuItemId);
}

void DrawSpecializesSummary(const char *operation, const SdfPath &path, const SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawSdfPathSummary<SdfSpecialize>("Specializes", operation, path, primSpec, menuItemId);
}

inline std::string GetSummary(const SdfReference &arc) { return arc.GetAssetPath(); }
inline std::string GetSummary(const SdfPayload &arc) { return arc.GetAssetPath(); }
inline std::string GetSummary(const SdfPath &arc) { return arc.GetString(); }

inline void DrawArcTypeMenuItems(const SdfPrimSpecHandle &primSpec, const SdfReference &ref) {
    DrawAssetPathMenuItems(primSpec, ref);
}
inline void DrawArcTypeMenuItems(const SdfPrimSpecHandle &primSpec, const SdfPayload &pay) {
    DrawAssetPathMenuItems(primSpec, pay);
}
inline void DrawArcTypeMenuItems(const SdfPrimSpecHandle &primSpec, const SdfInherit &inh) {
    DrawSdfPathMenuItems<SdfInherit>(primSpec, inh);
}
inline void DrawArcTypeMenuItems(const SdfPrimSpecHandle &primSpec, const SdfSpecialize &spe) {
    DrawSdfPathMenuItems<SdfSpecialize>(primSpec, spe);
}

template <typename ArcT>
inline
void DrawPathInRow(const char *operation, const ArcT &assetPath, const SdfPrimSpecHandle &primSpec, int *itemId) {
    std::string path;
    path += GetSummary(assetPath);
    ImGui::PushID((*itemId)++);
    ImGui::SameLine();
    if(ImGui::Button(path.c_str())) {
        InspectArcType(primSpec, assetPath);
    }
    if (ImGui::BeginPopupContextItem("###AssetPathMenuItems")) {
        DrawArcTypeMenuItems(primSpec, assetPath);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

#define CREATE_COMPOSITION_BUTTON(NAME_, ABBR_, GETLIST_)                                                                        \
if (primSpec->Has##NAME_##s()) {                                                                                             \
    if (buttonId > 0)                                                                                                        \
        ImGui::SameLine();                                                                                                   \
    ImGui::PushID(buttonId++);                                                                                               \
    ImGui::SmallButton(#ABBR_);                                                                                              \
    if (ImGui::BeginPopupContextItem(nullptr, buttonFlags)) {                                                                \
        IterateListEditorItems(primSpec->Get##GETLIST_##List(), Draw##GETLIST_##Summary, primSpec, buttonId);                \
        ImGui::EndPopup();                                                                                                   \
    }                                                                                                                        \
    ImGui::PopID();                                                                                                          \
}

void DrawPrimCompositionSummary(const SdfPrimSpecHandle &primSpec) {
    if (!primSpec || !HasComposition(primSpec))
        return;
    ScopedStyleColor transparentStyle(ImGuiCol_Button, ImVec4(ColorTransparent));
    
    // Buttons are too far appart
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, -FLT_MIN));
    int buttonId = 0;
    // First draw the Buttons, Ref, Pay etc
    constexpr ImGuiPopupFlags buttonFlags = ImGuiPopupFlags_MouseButtonLeft;
    
    CREATE_COMPOSITION_BUTTON(Reference, Ref, Reference)
    CREATE_COMPOSITION_BUTTON(Payload, Pay, Payload)
    CREATE_COMPOSITION_BUTTON(InheritPath, Inh, InheritPath)
    CREATE_COMPOSITION_BUTTON(Specialize, Inh, Specializes)

    // TODO: stretch each paths to match the cell size. Add ellipsis at the beginning if they are too short
    // The idea is to see the relevant informations, and be able to quicly click on them
    // - another thought ... replace the common prefix by an ellipsis ? (only for asset paths)
    int itemId = 0;
    IterateListEditorItems(primSpec->GetReferenceList(), DrawPathInRow<SdfReference>, primSpec, &itemId);
    IterateListEditorItems(primSpec->GetPayloadList(), DrawPathInRow<SdfPayload>, primSpec, &itemId);
    IterateListEditorItems(primSpec->GetInheritPathList(), DrawPathInRow<SdfInherit>, primSpec, &itemId);
    IterateListEditorItems(primSpec->GetSpecializesList(), DrawPathInRow<SdfSpecialize>, primSpec, &itemId);
    ImGui::PopStyleVar();
}
