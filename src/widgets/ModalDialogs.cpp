
#include "Gui.h"
#include "ModalDialogs.h"

/// The current and only modal dialog to display
static ModalDialog *currentModalDialog = nullptr;
bool modalOpenTriggered = false;
bool modalCloseTriggered = false;

ModalDialog *GetCurrentModalDialog() { return currentModalDialog; };

void SetCurrentModalDialog(ModalDialog *dialog) { currentModalDialog = dialog; }

bool ShouldOpenModal() {
    if (!currentModalDialog) {
        return false;
    } else if (modalOpenTriggered) {
        modalOpenTriggered = false;
        return true;
    } else {
        return false;
    }
}

void ModalDialog::CloseModal() {
    modalCloseTriggered = true; // Delete the dialog at the next frame;
    ImGui::CloseCurrentPopup();
}

void CheckCloseModal() {
    if (modalCloseTriggered) {
        modalCloseTriggered = false;
        delete currentModalDialog;
        currentModalDialog = nullptr;
    }
}

void ForceCloseCurrentModal() {
    if (currentModalDialog) {
        currentModalDialog->CloseModal();
        CheckCloseModal();
    }
}

void DrawCurrentModal() {
    CheckCloseModal();
    if (ShouldOpenModal()) {
        ImGui::OpenPopup(currentModalDialog->DialogId());
    }
    if (currentModalDialog && ImGui::BeginPopupModal(currentModalDialog->DialogId())) {
        currentModalDialog->Draw();
        ImGui::EndPopup();
    }
}

void DrawOkCancelModal(const std::function<void()> &onOk) {
    // Align right
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetWindowWidth() - 3 * ImGui::CalcTextSize(" Cancel ").x -
                         ImGui::GetScrollX() - 2 * ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(" Cancel ")) {
        currentModalDialog->CloseModal();
    }
    ImGui::SameLine();
    if (ImGui::Button("   Ok   ")) {
        onOk();
        currentModalDialog->CloseModal();
    }
}
