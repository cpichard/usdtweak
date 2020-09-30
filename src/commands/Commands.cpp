#include "Commands.h"
#include "SdfCommandGroup.h"
#include "SdfUndoRecorder.h"
#include <functional>
#include <iostream>
#include <vector>

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
/// Inherit from SdfLayerCommand if your command is changing the layer.
/// then in DoIt() instanciate an SdfUndoRecorder:
///    SdfUndoRecorder recorder(_undoCommands, _layer);
/// It will record the undo commands
///
struct SdfLayerCommand : public Command {
    virtual ~SdfLayerCommand(){};
    virtual bool DoIt() = 0;
    bool UndoIt() override {
        _undoCommands();
        return false;
    }

    SdfCommandGroup _undoCommands;
};

// The undo stacl should ultimately belong to an Editor, not be set as a global variable
using UndoStackT = std::vector<std::unique_ptr<Command>>;
static UndoStackT undoStack;

// Storing only one command per frame for now
static Command *lastCmd = nullptr;

/// The pointer to the current command in the undo stack
static int undoStackPos = 0;

/// Dispatching a command from the software will create a command but not Run it.
template <typename CommandClass, typename... ArgTypes> void DispatchCommand(ArgTypes... arguments) {
    if (!lastCmd) {
        lastCmd = new CommandClass(arguments...);
    }
}

/// The ProcessCommands function is called after the frame is rendered and displayed and execute the
/// last command.
void ProcessCommands() {
    if (lastCmd) {
        if (lastCmd->DoIt()) {
            if (undoStackPos != undoStack.size()) {
                undoStack.resize(undoStackPos);
            }
            undoStack.emplace_back(std::move(lastCmd));
            undoStackPos++;
        }
       lastCmd = nullptr;   // Reset the command
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

