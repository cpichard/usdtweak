
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/payload.h>
#include "CompositionEditor.h"
#include "Gui.h"
#include "ProxyHelpers.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "Commands.h"
#include "ImGuiHelpers.h"

// 2 local struct to differentiate Inherit and Specialize which have the same underlying type
struct Inherit {};
struct Specialize {};

static bool HasComposition(const SdfPrimSpecHandle &primSpec) {
    return primSpec->HasReferences() || primSpec->HasPayloads() || primSpec->HasInheritPaths() || primSpec->HasSpecializes();
}

/// Create a standard UI for entering a SdfPath.
/// This is used for inherit and specialize
struct CreateSdfPathModalDialog : public ModalDialog {

    CreateSdfPathModalDialog(SdfPrimSpecHandle &primSpec) : _primSpec(primSpec){};
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
                if (ImGui::Selectable(GetListEditorOperationName(n))) {
                    _operation = n;
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
    int _operation = 0;
};

/// UI used to create an AssetPath having a file path to a layer and a
/// target prim path.
/// This is used by References and Payloads which share the same interface
struct CreateAssetPathModalDialog : public ModalDialog {

    CreateAssetPathModalDialog(SdfPrimSpecHandle &primSpec) : _primSpec(primSpec){};
    ~CreateAssetPathModalDialog() override {}

    void Draw() override {
        if (!_primSpec) {
            CloseModal();
            return;
        }
        ImGui::Text("%s", _primSpec->GetPath().GetString().c_str());
        if (fileBrowser) {
            if (ImGui::Button(ICON_FA_FILE)) {
                fileBrowser = !fileBrowser;
            }
            ImGui::SameLine();
            DrawFileBrowser();
            if (ImGui::Button("Use selected file")) {
                fileBrowser = !fileBrowser;
                _assetPath = GetFileBrowserFilePath();
            }
        } else {
            if (ImGui::BeginCombo("Operation", GetListEditorOperationName(_operation))) {
                for (int n = 0; n < GetListEditorOperationSize(); n++) {
                    if (ImGui::Selectable(GetListEditorOperationName(n))) {
                        _operation = n;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("File path", &_assetPath);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FILE)) {
                fileBrowser = !fileBrowser;
            }
            ImGui::InputText("Target prim path", &_primPath);
            DrawOkCancelModal([=]() { OnOkCallBack(); });
        }
    }

    virtual void OnOkCallBack() = 0;

    const char *DialogId() const override { return "Asset path"; }

    SdfPrimSpecHandle _primSpec;
    std::string _assetPath;
    std::string _primPath;
    // SdfLayerOffset layerOffset; // TODO
    int _operation = 0;
    bool fileBrowser = false;
};

// Inheriting, but could also be done with templates, would the code be cleaner ?
struct CreateReferenceModalDialog : public CreateAssetPathModalDialog {
    CreateReferenceModalDialog(SdfPrimSpecHandle &primSpec) : CreateAssetPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create reference"; }
    void OnOkCallBack() override {
        SdfReference reference(_assetPath, SdfPath(_primPath));
        ExecuteAfterDraw<PrimCreateReference>(_primSpec, _operation, reference);
    }
};

struct CreatePayloadModalDialog : public CreateAssetPathModalDialog {
    CreatePayloadModalDialog(SdfPrimSpecHandle &primSpec) : CreateAssetPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create payload"; }
    void OnOkCallBack() override {
        SdfPayload payload(_assetPath, SdfPath(_primPath));
        ExecuteAfterDraw<PrimCreatePayload>(_primSpec, _operation, payload);
    }
};

struct CreateInheritModalDialog : public CreateSdfPathModalDialog {
    CreateInheritModalDialog(SdfPrimSpecHandle &primSpec) : CreateSdfPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create inherit"; }
    void OnOkCallBack() override { ExecuteAfterDraw<PrimCreateInherit>(_primSpec, _operation, SdfPath(_primPath)); }
};

struct CreateSpecializeModalDialog : public CreateSdfPathModalDialog {
    CreateSpecializeModalDialog(SdfPrimSpecHandle &primSpec) : CreateSdfPathModalDialog(primSpec) {}
    const char *DialogId() const override { return "Create specialize"; }
    void OnOkCallBack() override { ExecuteAfterDraw<PrimCreateSpecialize>(_primSpec, _operation, SdfPath(_primPath)); }
};

void DrawPrimCreateReference(SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreateReferenceModalDialog>(primSpec); }
void DrawPrimCreatePayload(SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreatePayloadModalDialog>(primSpec); }
void DrawPrimCreateInherit(SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreateInheritModalDialog>(primSpec); }
void DrawPrimCreateSpecialize(SdfPrimSpecHandle &primSpec) { DrawModalDialog<CreateSpecializeModalDialog>(primSpec); }

/// Remove a Asset Path from a primspec
inline void RemoveAssetPathFromList(SdfPrimSpecHandle primSpec, const SdfReference &item) {
    primSpec->GetReferenceList().RemoveItemEdits(item);
}
inline void RemoveAssetPathFromList(SdfPrimSpecHandle primSpec, const SdfPayload &item) {
    primSpec->GetPayloadList().RemoveItemEdits(item);
}

/// Remove SdfPath from their lists (Inherits and Specialize)
template <typename ListT> inline void RemovePathFromList(SdfPrimSpecHandle primSpec, const SdfPath &item);
template <> inline void RemovePathFromList<Inherit>(SdfPrimSpecHandle primSpec, const SdfPath &item) {
    primSpec->GetInheritPathList().RemoveItemEdits(item);
}
template <> inline void RemovePathFromList<Specialize>(SdfPrimSpecHandle primSpec, const SdfPath &item) {
    primSpec->GetSpecializesList().RemoveItemEdits(item);
}

///
template <typename PathOriginT> void DrawSdfPathMenuItems(const SdfPrimSpecHandle &primSpec, const SdfPath &path) {
    if (ImGui::MenuItem("Remove")) {
        if (!primSpec)
            return;
        std::function<void()> removeAssetPath = [=]() { RemovePathFromList<PathOriginT>(primSpec, path); };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removeAssetPath);
    }
    if (ImGui::MenuItem("Copy path")) {
        ImGui::SetClipboardText(path.GetString().c_str());
    }
}

/// Draw the menu items for AssetPaths (SdfReference and SdfPayload)
template <typename AssetPathT> void DrawAssetPathMenuItems(const SdfPrimSpecHandle &primSpec, const AssetPathT &assetPath) {

    if (ImGui::MenuItem("Inspect")) {
        ExecuteAfterDraw<EditorOpenLayer>(assetPath.GetAssetPath());
    }
    if (ImGui::MenuItem("Open as Stage")) {
        ExecuteAfterDraw<EditorOpenStage>(assetPath.GetAssetPath());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Remove")) {
        // I am not 100% sure this is safe as we copy the primSpec instead of its location
        // The command UsdFunctionCall will store it between here and the actual call, so between this time it
        // might be invalidated by another process. Unlikely but possible
        // TODO: make a command that stores the location of the prim
        if (!primSpec)
            return;
        std::function<void()> removeAssetPath = [=]() { RemoveAssetPathFromList(primSpec, assetPath); };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removeAssetPath);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy asset path")) {
        ImGui::SetClipboardText(assetPath.GetAssetPath().c_str());
    }
}


template <typename ListT>
static void DrawSdfPathRow(const char *operationName, const SdfPath &path, SdfPrimSpecHandle &primSpec) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (ImGui::SmallButton(ICON_FA_TRASH)) { // TODO: replace with the proper mini button menu
        if (!primSpec)
            return;
        // DUPLICATED CODE that should go away with the mini button menu
        std::function<void()> removePath = [=]() { RemovePathFromList<ListT>(primSpec, path); };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removePath);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", operationName);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%s", path.GetString().c_str());
    if (ImGui::BeginPopupContextItem()) {
        DrawSdfPathMenuItems<ListT>(primSpec, path);
        ImGui::EndPopup();
    }
}

// Draw a row in a table for SdfReference or SdfPayload
// It's templated to keep the formatting consistent between payloads and references
// and avoid to duplicate the code
template <typename AssetPathT>
static void DrawAssetPathRow(const char *operationName, const AssetPathT &item,
                             SdfPrimSpecHandle &primSpec) { // TODO:primSpec might not be useful here
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (ImGui::SmallButton(ICON_FA_TRASH)) { // TODO: replace with the proper menu
        if (!primSpec)
            return;
        std::function<void()> removeAssetPath = [=]() { RemoveAssetPathFromList(primSpec, item); };
        ExecuteAfterDraw<UsdFunctionCall>(primSpec->GetLayer(), removeAssetPath);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", operationName);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%s", item.GetAssetPath().c_str());
    if (ImGui::BeginPopupContextItem(item.GetPrimPath().GetString().c_str())) {
        DrawAssetPathMenuItems(primSpec, item);
        ImGui::EndPopup();
    }
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%s", item.GetPrimPath().GetString().c_str());
}

void DrawPrimPayloads(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasPayloads())
        return;
    if (ImGui::BeginTable("##DrawPrimPayloads", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        TableSetupColumns("", "", "Payloads", "");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetPayloadList(), DrawAssetPathRow<SdfPayload>, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimReferences(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasReferences())
        return;
    if (ImGui::BeginTable("##DrawPrimReferences", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        TableSetupColumns("", "", "References", "");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetReferenceList(), DrawAssetPathRow<SdfReference>, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimInherits(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasInheritPaths())
        return;
    if (ImGui::BeginTable("##DrawPrimInherits", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        TableSetupColumns("", "", "Inherit");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetInheritPathList(), DrawSdfPathRow<Inherit>, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimSpecializes(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasSpecializes())
        return;
    if (ImGui::BeginTable("##DrawPrimSpecializes", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        TableSetupColumns("", "", "Specialize");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetSpecializesList(), DrawSdfPathRow<Specialize>, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimCompositions(SdfPrimSpecHandle &primSpec) {
    if (!primSpec || !HasComposition(primSpec))
        return;

    DrawPrimReferences(primSpec);
    DrawPrimPayloads(primSpec);
    DrawPrimInherits(primSpec);
    DrawPrimSpecializes(primSpec);
}

/////////////// Summary for the layer editor

template <typename PathListT>
inline void DrawSdfPathSummary(std::string &&header, const char *operation, const SdfPath &path, SdfPrimSpecHandle &primSpec,
                               int &menuItemId) {
    ImGui::PushID(menuItemId++);
    std::string summary = header + path.GetString(); // TODO: add target prim and offsets
    ImGui::Selectable(summary.c_str());
    if (ImGui::BeginPopupContextItem()) {
        DrawSdfPathMenuItems<PathListT>(primSpec, path);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

template <typename AssetPathT>
inline void DrawAssetPathSummary(std::string &&header, const char *operation, const AssetPathT &assetPath,
                                 SdfPrimSpecHandle &primSpec, int &menuItemId) {
    ImGui::PushID(menuItemId++);
    std::string summary = header + assetPath.GetAssetPath(); // TODO: add target prim and offsets
    ImGui::Selectable(summary.c_str());
    if (ImGui::BeginPopupContextItem()) {
        DrawAssetPathMenuItems(primSpec, assetPath);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void DrawReferenceSummary(const char *operation, const SdfReference &assetPath, SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawAssetPathSummary(ICON_FA_EXTERNAL_LINK_ALT " reference ", operation, assetPath, primSpec, menuItemId);
}

void DrawPayloadSummary(const char *operation, const SdfPayload &assetPath, SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawAssetPathSummary(ICON_FA_BULLSEYE " payload ", operation, assetPath, primSpec, menuItemId);
}

void DrawInheritsSummary(const char *operation, const SdfPath &path, SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawSdfPathSummary<Inherit>(ICON_FA_EXTERNAL_LINK_SQUARE_ALT " inherit ", operation, path, primSpec, menuItemId);
}

void DrawSpecializesSummary(const char *operation, const SdfPath &path, SdfPrimSpecHandle &primSpec, int &menuItemId) {
    DrawSdfPathSummary<Specialize>(ICON_FA_SIGN_IN_ALT " specialize ", operation, path, primSpec, menuItemId);
}

void DrawPrimCompositionSummary(SdfPrimSpecHandle &primSpec) {
    if (!primSpec || !HasComposition(primSpec))
        return;
    int menuItemId = 0;
    IterateListEditorItems(primSpec->GetReferenceList(), DrawReferenceSummary, primSpec, menuItemId);
    IterateListEditorItems(primSpec->GetPayloadList(), DrawPayloadSummary, primSpec, menuItemId);
    IterateListEditorItems(primSpec->GetInheritPathList(), DrawInheritsSummary, primSpec, menuItemId);
    IterateListEditorItems(primSpec->GetSpecializesList(), DrawSpecializesSummary, primSpec, menuItemId);
}
