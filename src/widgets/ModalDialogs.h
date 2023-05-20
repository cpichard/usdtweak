#pragma once
#include <functional>



// A modal dialog should know how to draw itself
struct ModalDialog {
    virtual void Draw() = 0;
    virtual ~ModalDialog(){};
    virtual const char *DialogId() const = 0;
    static void CloseModal();
};

/// This is a private variable and function for DrawModalDialog don't use them !
extern bool modalOpenTriggered;
void _PushModalDialog(ModalDialog *);

/// Trigger a modal dialog
template <typename T, typename... ArgTypes> void DrawModalDialog(ArgTypes &&... args) {
    modalOpenTriggered = true;
    _PushModalDialog(new T(args...));
}

/// Draw the current modal dialog if it has been triggered
void DrawCurrentModal();

/// Convenience function to draw an Ok and Cancel buttons in a Modal dialog
void DrawOkCancelModal(const std::function<void()> &onOk, bool disableOk=false);

/// Force closing the current modal dialog
void ForceCloseCurrentModal();
