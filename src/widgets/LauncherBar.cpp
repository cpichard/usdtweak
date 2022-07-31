#include "Commands.h"
#include "Editor.h"
#include "Gui.h"
#include "LauncherBar.h"
#include "ModalDialogs.h"

/// Very basic ui to create a connection
struct AddLauncherDialog : public ModalDialog {

    AddLauncherDialog(){};
    ~AddLauncherDialog() override {}

    void Draw() override {
        ImGui::InputText("Launcher name", &_launcherName);
        ImGui::InputText("Command line", &_commandLine);
        DrawOkCancelModal([&]() { ExecuteAfterDraw<EditorAddLauncher>(_launcherName, _commandLine); });
    }
    const char *DialogId() const override { return "Add launcher"; }

    std::string _launcherName;
    std::string _commandLine;
};

void DrawLauncherBar(Editor *editor) {
    if (!editor)
        return;

    if (ImGui::Button("+")) {
        DrawModalDialog<AddLauncherDialog>();
    }

    ImGui::SameLine();
    for (const auto &commandName : editor->GetLauncherNameList()) {
        if (ImGui::Button(commandName.c_str())) {
            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                ExecuteAfterDraw<EditorRemoveLauncher>(commandName);
            } else {
                ExecuteAfterDraw<EditorRunLauncher>(commandName);
            }
        }
        ImGui::SameLine();
    }
    // TODO hint to say that control click deletes the launcher
}
