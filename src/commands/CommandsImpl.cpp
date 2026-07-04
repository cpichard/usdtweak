#include "CommandStack.h"
#include "CommandsImpl.h"
#include "SdfCommandGroup.h"
#include "SdfCommandGroupRecorder.h"
#include "SdfUndoRedoRecorder.h"
#include "UndoLayerStateDelegate.h"
#include "UsdSceneLock.h"
#include <functional>
#include <vector>

bool SdfLayerCommand::UndoIt() {
    _undoCommands.UndoIt();
    return false;
}

bool SdfUndoRedoCommand::UndoIt() {
    _undoCommands.UndoIt();
    return false;
}

bool SdfUndoRedoCommand::DoIt() {
    _undoCommands.DoIt();
    return true;
}
namespace {
SdfUndoRedoRecorder *undoRedoRecorder = nullptr;
}

void BeginEdition(SdfLayerRefPtr layer) {
    if (layer) {
        if (undoRedoRecorder) // already recording — keep lock/recorder pairing exact
            return;
        // The Begin/End span is the direct UI-thread write path (manipulator
        // drags and widget sliders author every frame, outside the command
        // queue). Hold the exclusive scene lock for the whole span so
        // background readers never observe the scene mid-drag. The lock is
        // reentrant: commands executed during the span still work.
        UsdSceneLock::GetInstance().LockWrite();
        undoRedoRecorder = new SdfUndoRedoRecorder(layer);
        undoRedoRecorder->StartRecording();
    }
}
///
void BeginEdition(UsdStageRefPtr stage) {
    // Get layer
    if (stage) {
        BeginEdition(stage->GetEditTarget().GetLayer());
    }
}

void EndEdition() {
    if (undoRedoRecorder) {
        undoRedoRecorder->StopRecording();
        delete undoRedoRecorder;
        undoRedoRecorder = nullptr;
        UsdSceneLock::GetInstance().UnlockWrite();
    }
}

// Include all the commands as cpp files to compile them with this unit as we want to have
// at least two implementation, one for the Editor and another for a widget library.
// We can create a CommandsUndoRedoImpl.cpp and CommandsUndoRedoImpl.tpp later on
// and another version without undo/redo for the widget library:
// CommandsLibraryImpl.cpp and CommandsLibraryImpl.tpp
//#include "UndoRedoCommands.cpp"
//#include "PrimCommands.cpp"
//#include "EditorCommands.cpp"//
//#include "LayerCommands.cpp"
//#include "AttributeCommands.cpp"
