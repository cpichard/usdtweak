
#include "Gui.h"
#include "ModalDialogs.h"
#include <vector>

// Stack of modal dialogs
std::vector<ModalDialog *> modalDialogStack;

void _PushModalDialog(ModalDialog *modal) { modalDialogStack.push_back(modal); }

bool modalOpenTriggered = false;
bool modalCloseTriggered = false;

bool ShouldOpenModal() {
    if (modalDialogStack.empty()) {
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
    ImGui::CloseCurrentPopup(); // this is called in the draw function
}

void CheckCloseModal() {
    if (modalCloseTriggered) {
        modalCloseTriggered = false;
        delete modalDialogStack.back();
        modalDialogStack.pop_back();
    }
}

void ForceCloseCurrentModal() {
    if (!modalDialogStack.empty()) {
        modalDialogStack.back()->CloseModal();
        CheckCloseModal();
    }
}

static void BeginPopupModalRecursive(const std::vector<ModalDialog *> &modals, int index) {
    if (index < modals.size()) {
        ModalDialog *modal = modals[index];
        if (index == modals.size() - 1) {
            if (ShouldOpenModal()) {
                ImGui::OpenPopup(modal->DialogId());
            }
        }
        if (ImGui::BeginPopupModal(modal->DialogId())) {
            modal->Draw();
            BeginPopupModalRecursive(modals, index + 1);
            ImGui::EndPopup();
        }
    }
}

//
// DrawCurrentModal will check if there is anything to action
// like closing or draw ... it should really be called ProcessModalDialogs
void DrawCurrentModal() {
    CheckCloseModal();
    BeginPopupModalRecursive(modalDialogStack, 0);
}

void DrawOkCancelModal(const std::function<void()> &onOk, bool disableOk) {
    // Align right
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetWindowWidth() - 3 * ImGui::CalcTextSize(" Cancel ").x -
                         ImGui::GetScrollX() - 2 * ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(" Cancel ")) {
        modalDialogStack.back()->CloseModal();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(disableOk);
    if (ImGui::Button("   Ok   ")) {
        onOk();
        modalDialogStack.back()->CloseModal();
    }
    ImGui::EndDisabled();
}
