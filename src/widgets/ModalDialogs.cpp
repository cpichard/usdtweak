
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
            modal->PrepareModal();
            if (ShouldOpenModal()) {
                ImGui::OpenPopup(modal->DialogId());
            }
        }
        const int styleCount = modal->PushStyles();
        const bool isOpen = ImGui::BeginPopupModal(modal->DialogId(), nullptr, modal->WindowFlags());
        ImGui::PopStyleVar(styleCount);
        if (isOpen) {
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

void DrawModalButtonsOkCancel(const std::function<void()> &onOk, bool disableOk) {
    // Stretch Ok - Cancel buttons
    const ImVec2 buttonSize(ImVec2(ImGui::GetWindowWidth()/2 - 2 * ImGui::GetStyle().ItemSpacing.x,
                                   ImGui::GetFrameHeight()));
    if (ImGui::Button("Cancel", buttonSize)) {
        modalDialogStack.back()->CloseModal();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(disableOk);
    if (ImGui::Button("Ok", buttonSize)) {
        onOk();
        modalDialogStack.back()->CloseModal();
    }
    ImGui::EndDisabled();
}

void DrawModalButtonClose() {
    const ImVec2 buttonSize(ImVec2(ImGui::GetWindowWidth() - 2 * ImGui::GetStyle().ItemSpacing.x,
                                   ImGui::GetFrameHeight()));
    if (ImGui::Button("Close", buttonSize)) {
        modalDialogStack.back()->CloseModal();
    }
}

