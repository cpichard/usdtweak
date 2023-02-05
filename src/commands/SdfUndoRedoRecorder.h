#pragma once

#include <functional>
#include <vector>
#include "CommandsImpl.h"
#include "SdfCommandGroup.h"
#include "SdfCommandGroupRecorder.h"
#include "UndoLayerStateDelegate.h"
#include "UndoRedoCommands.h"
#include "CommandStack.h"

/// A SdfUndoRedoRecorder creates an object on the stack which will start recording all the usd commands
/// and stops when it is destroyed.
/// It will also push the recorder commands on the stack
class SdfUndoRedoRecorder final {
public:

    SdfUndoRedoRecorder(SdfLayerRefPtr layer)
        : _editedCommand(nullptr), _layer(layer) {
    }
    ~SdfUndoRedoRecorder();

    void StartRecording();
    void StopRecording();

private:
    SdfUndoRedoCommand  *_editedCommand;
    SdfLayerRefPtr _layer;
    SdfLayerStateDelegateBaseRefPtr _previousDelegate;
};
