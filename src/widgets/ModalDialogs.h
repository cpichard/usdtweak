#pragma once
#include "imgui.h"
#include <functional>

// A modal dialog should know how to draw itself
struct ModalDialog {
    virtual void Draw() = 0;
    virtual ~ModalDialog(){};
    virtual const char *DialogId() const = 0;
    // Override to pass custom window flags (e.g. NoTitleBar) to BeginPopupModal
    virtual ImGuiWindowFlags WindowFlags() const { return ImGuiWindowFlags_None; }
    // Override to call SetNextWindowSize/Pos before BeginPopupModal is called
    virtual void PrepareModal() {}
    // Override to push ImGui style vars before BeginPopupModal; return the count pushed.
    // The caller will pop them immediately after BeginPopupModal.
    virtual int PushStyles() { return 0; }
    static void CloseModal();
};

/// This is a private variable and function for DrawModalDialog don't use them !
extern bool modalOpenTriggered;
void _PushModalDialog(ModalDialog *);

/// Trigger a modal dialog
template <typename T, typename... ArgTypes> void DrawModalDialog(ArgTypes &&...args) {
    modalOpenTriggered = true;
    _PushModalDialog(new T(args...));
}

/// Draw the current modal dialog if it has been triggered
void DrawCurrentModal();

/// Convenience function to draw an Ok and Cancel buttons in a Modal dialog
void DrawModalButtonsOkCancel(const std::function<void()> &onOk, bool disableOk = false);

/// Convenience function to draw an Close button in a Modal dialog
void DrawModalButtonClose();

/// Force closing the current modal dialog
void ForceCloseCurrentModal();
