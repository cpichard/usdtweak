#include "SdfUndoRecorder.h"
#include "UndoLayerStateDelegate.h"
#include <iostream>

SdfUndoRecorder::SdfUndoRecorder(SdfCommandGroup &undoCommands, SdfLayerRefPtr layer)
    : _undoCommands(undoCommands), _layer(layer) {
    if (_layer){
        _previousDelegate = _layer->GetStateDelegate();
        if (undoCommands.IsEmpty()) { // No undo commands were previously recorded
            std::cout << "Recording commands" << std::endl;
            _layer->SetStateDelegate(UndoLayerStateDelegate::New(undoCommands));
        }
    }
}

SdfUndoRecorder::~SdfUndoRecorder() {
    if (_layer && _previousDelegate) {
        _layer->SetStateDelegate(_previousDelegate);
    }
}
