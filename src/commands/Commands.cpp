#include "Commands.h"
#include <functional>
#include <iostream>
#include <vector>

/// Base class for all commands.
struct Command {
    virtual ~Command(){};
    virtual bool DoIt() = 0;
    virtual bool UndoIt() { return false; }
};

// This should ultimately belong to an editor
using UndoStackT = std::vector<std::unique_ptr<Command>>;

// Storing only one command per frame for now
static Command *lastCmd = nullptr;
static UndoStackT undoStack;

/// The pointer to the current command
static int undoStackPos = 0;

template <typename CommandClass, typename... ArgTypes> void DispatchCommand(ArgTypes... arguments) {
    if (!lastCmd) {
        lastCmd = new CommandClass(arguments...);
    }
}

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

// Include all the commands as cpp files to compile them with this unit
// We can create a CommandsUndoRedoImpl.cpp and CommandsUndoRedoImpl.tpp later on
// and another version without undo/redo for the widget library:
// CommandsLibraryImpl.cpp and CommandsLibraryImpl.tpp
#include "UndoRedoCommands.cpp"
#include "PrimCommands.cpp"
#include "EditorCommands.cpp"
#include "LayerCommands.cpp"

