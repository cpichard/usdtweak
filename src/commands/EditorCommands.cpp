///
/// This file should be included in Commands.cpp to be compiled with it.
/// IT SHOULD NOT BE COMPILED SEPARATELY
///

///
/// Work in progress, this is not used yet
///

#include <string>
#include "Commands.h"
#include "Editor.h"

struct EditorSelectPrimPath : public Command {

    EditorSelectPrimPath(SdfPath *selectionObject, SdfPath selectedPath)
        : selectionObject(selectionObject), selectedPath(std::move(selectedPath)) {}

    ~EditorSelectPrimPath() override {}

    // TODO: wip, we want an "Selection" object to be passed around
    // At the moment it is just the pointer to the current selection held by the editor
    bool DoIt() override {
        *selectionObject = selectedPath;
        return true;
    }
    SdfPath *selectionObject;
    SdfPath selectedPath;
};

template void DispatchCommand<EditorSelectPrimPath>(SdfPath *selectionObject, SdfPath selectedPath);


struct EditorOpenStage : public Command {

    EditorOpenStage(Editor *editor, std::string stagePath) : _editor(editor), _stagePath(std::move(stagePath)) {}
    ~EditorOpenStage() override {}

    bool DoIt() override {
        _editor->ImportStage(_stagePath);
        return true;
    }

    std::string _stagePath;
    Editor *_editor; /// Warning: this may outlive the editor life
};

template void DispatchCommand<EditorOpenStage>(Editor *editor, std::string stagePath);
