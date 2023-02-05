#include <iostream>
#include "SdfCommandGroupRecorder.h"
#include "UndoLayerStateDelegate.h"

SdfCommandGroupRecorder::SdfCommandGroupRecorder(SdfCommandGroup &undoCommands, SdfLayerRefPtr layer)
    : _undoCommands(undoCommands), _layer(layer) {
    if (_layer) {
        _previousDelegate = _layer->GetStateDelegate();
        if (undoCommands.IsEmpty()) { // No undo commands were previously recorded
            _layer->SetStateDelegate(UndoRedoLayerStateDelegate::New(undoCommands));
        }
    }
}

SdfCommandGroupRecorder::~SdfCommandGroupRecorder() {
    if (_layer && _previousDelegate) {
        _layer->SetStateDelegate(_previousDelegate);
    }
}
