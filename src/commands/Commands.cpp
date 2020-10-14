#include "Commands.h"
#include "SdfCommandGroup.h"
#include "SdfUndoRecorder.h"
#include <functional>
#include <iostream>
#include <vector>
#include "UndoLayerStateDelegate.h"

/// Base class for all commands.
/// As we expect to store lots of commands, it might be worth avoiding
/// inheritance, or at least not store the command but a serialization of it.
/// Specially with the SdfCommandGroups which allocates a vector for each CommandGroup
struct Command {
    virtual ~Command(){};
    virtual bool DoIt() = 0;
    virtual bool UndoIt() { return false; }
};

///
/// Inherit from SdfLayerCommand if your command is changing data in the sdflayer.
/// In DoIt() create an SdfUndoRecorder:
///    SdfUndoRecorder recorder(_undoCommands, _layer);
/// It will record undo commands for the commands run after
///
struct SdfLayerCommand : public Command {
    virtual ~SdfLayerCommand(){};
    virtual bool DoIt() = 0;
    bool UndoIt() override {
        _undoCommands.UndoIt();
        return false;
    }

    SdfCommandGroup _undoCommands;
};


struct SdfUndoRedoCommand : public Command {

    bool UndoIt() override {
        _instructions.UndoIt();
        return false;
    }

    bool DoIt() override {
        _instructions.DoIt();
        return true;
    }

    // Pretty basic and storing redundant data
    SdfCommandGroup _instructions;
};



// The undo stack should ultimately belong to an Editor, not be a global variable
using UndoStackT = std::vector<std::unique_ptr<Command>>;
static UndoStackT undoStack;

/// The pointer to the current command in the undo stack
static int undoStackPos = 0;

// Storing only one command per frame for now, easier to reason about.
static Command *lastCmd = nullptr;


/// Dispatching a command from the software will create a command but not Run it.
template <typename CommandClass, typename... ArgTypes> void DispatchCommand(ArgTypes... arguments) {
    if (!lastCmd) {
        lastCmd = new CommandClass(arguments...);
    }
}

/// The ProcessCommands function is called after the frame is rendered and displayed and execute the
/// last command.
void _PushCommand(Command *cmd) {
    if (undoStackPos != undoStack.size()) {
        undoStack.resize(undoStackPos);
    }
    undoStack.emplace_back(std::move(cmd));
    undoStackPos++;
}

void ProcessCommands() {
    if (lastCmd) {
        if (lastCmd->DoIt()) {
            _PushCommand(lastCmd);
        }
        else {
            delete lastCmd;
        }
       lastCmd = nullptr;   // Reset the command
    }
}

///
/// TEST IN PROGRESS
///

class SdfUndoRedoRecorder final {
public:

    SdfUndoRedoRecorder(SdfLayerRefPtr layer)
        : _editedCommand(nullptr), _layer(layer) {
    }

    ~SdfUndoRedoRecorder() {
        if (_layer && _previousDelegate) {
            _layer->SetStateDelegate(_previousDelegate);
        }
        if (_editedCommand){
            _PushCommand(_editedCommand);
            _editedCommand = nullptr;
        }
    }

    void StartRecording() {
        if (_layer){
            _previousDelegate = _layer->GetStateDelegate();
            if (!_editedCommand) {
                _editedCommand = new SdfUndoRedoCommand();
            }
            // Install undo/redo delegate
            _layer->SetStateDelegate(UndoRedoLayerStateDelegate::New(_editedCommand->_instructions));
        }
    }

    void StopRecording() {
        if (_layer && _previousDelegate){
            _layer->SetStateDelegate(_previousDelegate);
        }
    }

private:
    SdfUndoRedoCommand  *_editedCommand;
    SdfLayerRefPtr _layer;
    SdfLayerStateDelegateBaseRefPtr _previousDelegate;
};

SdfUndoRedoRecorder *undoRedoRecorder = nullptr;


///
void BeginEdition(UsdStageRefPtr stage) {
    // Get layer
    if (stage) {
        BeginEdition(stage->GetEditTarget().GetLayer());
    }
}

void BeginEdition(SdfLayerRefPtr layer) {
    if (layer) {
        undoRedoRecorder = new SdfUndoRedoRecorder(layer);
        undoRedoRecorder->StartRecording();
    }
}


void EndEdition() {
    if (undoRedoRecorder) {
        undoRedoRecorder->StopRecording();
        delete undoRedoRecorder;
        undoRedoRecorder = nullptr;
    }
}



// Include all the commands as cpp files to compile them with this unit as we want to have
// at least two implementation, one for the Editor and another for a widget library.
// We can create a CommandsUndoRedoImpl.cpp and CommandsUndoRedoImpl.tpp later on
// and another version without undo/redo for the widget library:
// CommandsLibraryImpl.cpp and CommandsLibraryImpl.tpp
#include "UndoRedoCommands.cpp"
#include "PrimCommands.cpp"
#include "EditorCommands.cpp"
#include "LayerCommands.cpp"
#include "AttributeCommands.cpp"

