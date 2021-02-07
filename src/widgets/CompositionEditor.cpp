
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/payload.h>
#include "CompositionEditor.h"
#include "Gui.h"
#include "ProxyHelpers.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "Commands.h"

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
/// target prim path. This is used by References and Payloads which share the same
/// interface
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

static void DrawPathRow(const char *operationName, const SdfPath &path, SdfPrimSpecHandle &primSpec) {
    ImGui::TableNextRow();
    // TODO: mini button
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(operationName);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text(path.GetString().c_str());
}

void DrawPrimInherits(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasInheritPaths())
        return;
    if (ImGui::BeginTable("##DrawPrimInherits", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Inherit");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetInheritPathList(), DrawPathRow, primSpec);
        ImGui::EndTable();
    }
}

void DrawPrimSpecializes(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasSpecializes())
        return;
    if (ImGui::BeginTable("##DrawPrimSpecializes", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Specialize");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetSpecializesList(), DrawPathRow, primSpec);
        ImGui::EndTable();
    }
}

// Draw a row in a table for SdfReference or SdfPayload
// It's templated to keep the formatting consistent between payloads and references
// and avoid to duplicate the code
template <typename PathArcT>
static void DrawAssetPathRow(const char *operationName, const PathArcT &item, SdfPrimSpecHandle &primSpec) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::SmallButton("(c)");
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(operationName);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text(item.GetAssetPath().c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::Text(item.GetPrimPath().GetString().c_str());
}

void DrawPrimPayloads(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasPayloads())
        return;
    if (ImGui::BeginTable("##DrawPrimPayloads", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Payloads");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetPayloadList(), DrawAssetPathRow<SdfPayload>, primSpec);
        ImGui::EndTable();
    }
}

// TODO: could factor DrawPrimReferences and DrawPrimPayloads
// DrawPrimExternalArc<>
void DrawPrimReferences(SdfPrimSpecHandle &primSpec) {
    if (!primSpec->HasReferences())
        return;
    if (ImGui::BeginTable("##DrawPrimReferences", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24); // 24 => size of the mini button
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("References");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        IterateListEditorItems(primSpec->GetReferenceList(), DrawAssetPathRow<SdfReference>, primSpec);
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