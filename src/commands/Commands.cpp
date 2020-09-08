#include "Commands.h"
#include <functional>
#include <iostream>

/// Base class for all commands.
struct Command {
    virtual ~Command(){};
    virtual bool doIt() = 0;
    // virtual void undoIt() = 0; // one day, one day
};

// Storing only one command for now, no undo/redo as the architecture is still in flux
static Command *lastCmd = nullptr;

/// Memory used for storing the current and only command.
/// Avoid allocating memory for commands ...
/// This is probably not useful and definitely for fun right now
static char commandMemory[512]; // TODO: the size should be computed as the max of all commands size

template <typename CommandClass, typename... ArgTypes> void DispatchCommand(ArgTypes... arguments) {
    if (!lastCmd)
        lastCmd = new (&commandMemory[0]) CommandClass(arguments...);
}

void ProcessCommands() {
    if (lastCmd) {
        lastCmd->doIt();
        lastCmd->~Command(); // Need to explicitly call the destructor with a placement new
        lastCmd = nullptr;   // Reset the command
    }
}

// Include all the commands as cpp files to compile them with this unit
// We can later on create a CommandsUndoRedoImpl.cpp and CommandsUndoRedoImpl.tpp
// and another version without undo/redo for the widget library:
// CommandsLibraryImpl.cpp and CommandsLibraryImpl.tpp
#include "PrimCommands.cpp"
#include "EditorCommands.cpp"
#include "LayerCommands.cpp"

