#pragma once

extern bool modalOpenTriggered;

// A modal dialog should know how to draw itself
struct ModalDialog {
    virtual void Draw() = 0;
    virtual ~ModalDialog(){};
    virtual const char *DialogId() const = 0;
};

/// TODO: Get/Set CurrentModalDialog shouldn't be exposed in this header
ModalDialog *GetCurrentModalDialog();
void SetCurrentModalDialog(ModalDialog *);

/// Trigger a modal dialog
template <typename T, typename... ArgTypes> void DrawModalDialog(ArgTypes &&... args) {
    modalOpenTriggered = true;
    if (!GetCurrentModalDialog()) {
        SetCurrentModalDialog(new T(args...));
    }
}

/// Draw the current modal dialog if it has been triggered
void DrawCurrentModal();

/// Close the modal
void CloseModal();
