///
/// This file should be included in Commands.cpp to be compiled with it.
/// IT SHOULD NOT BE COMPILED SEPARATELY
///
#include "Commands.h"
#include "Editor.h"

struct EditorSelectPrimPath : public Command {

    EditorSelectPrimPath(SdfPath *selectionObject, SdfPath selectedPath)
        : selectionObject(selectionObject), selectedPath(std::move(selectedPath)) {}

    ~EditorSelectPrimPath() override {}


    // TODO: wip, we want an "Selection" object to be passed around
    // At the moment it is just the pointer to the current selection held by the editor
    bool doIt() override {
        *selectionObject = selectedPath;
        return true;
    }
    SdfPath *selectionObject;
    SdfPath selectedPath;
};

template void DispatchCommand<EditorSelectPrimPath>(SdfPath *selectionObject, SdfPath selectedPath);
