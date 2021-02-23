#include <iostream>
#include "SdfUndoRecorder.h"
#include "UndoLayerStateDelegate.h"

SdfUndoRecorder::SdfUndoRecorder(SdfCommandGroup &undoCommands, SdfLayerRefPtr layer)
    : _undoCommands(undoCommands), _layer(layer) {
    if (_layer) {
        _previousDelegate = _layer->GetStateDelegate();
        if (undoCommands.IsEmpty()) { // No undo commands were previously recorded
            _layer->SetStateDelegate(UndoRedoLayerStateDelegate::New(undoCommands));
        }
    }
}

SdfUndoRecorder::~SdfUndoRecorder() {
    if (_layer && _previousDelegate) {
        _layer->SetStateDelegate(_previousDelegate);
    }
}
