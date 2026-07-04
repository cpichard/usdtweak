#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "CommandsImpl.h"

struct CommandStack {

    // Undo and Redo calls are implemented as commands.
    // We compile them in the CommandStack.cpp unit
    friend struct UndoCommand;
    friend struct RedoCommand;
    // Same for ClearUndoRedo
    friend struct ClearUndoRedoCommand;
    
    //
    friend struct UsdFunctionCall;
    friend struct MultiLayerFunctionCall;
    friend class SdfUndoRedoRecorder;
    
    static CommandStack &GetInstance();

    // The one-command-per-frame slot is filled from BOTH the UI thread
    // (widgets) and background workers (the Twiki agent's edit tools), so all
    // access goes through _lastCmdMutex.
    inline bool HasNextCommand() {
        std::lock_guard<std::mutex> lock(_lastCmdMutex);
        return lastCmd != nullptr;
    }
    /// Claim the slot if it is empty. Returns false (caller keeps ownership
    /// of `command`) when another command is already queued for this frame.
    inline bool TrySetNextCommand(Command *command) {
        std::lock_guard<std::mutex> lock(_lastCmdMutex);
        if (lastCmd)
            return false;
        lastCmd = command;
        return true;
    }

    // Execute next command and push it on the stack
    void ExecuteCommands();

private:


    // The undo stack should ultimately belong to an Editor, not be a global variable
    using UndoStackT = std::vector<std::unique_ptr<Command>>;
    UndoStackT undoStack;

    /// The pointer to the current command in the undo stack
    int undoStackPos = 0;

    // Storing only one command per frame for now, easier to reason about.
    Command *lastCmd = nullptr;
    std::mutex _lastCmdMutex; ///< guards lastCmd (UI thread vs agent worker)

    /// The ProcessCommands function is called after the frame is rendered and displayed and execute the
    /// last command. The command passed here now belongs to this stack
    void _PushCommand(Command *cmd);

  private:
    CommandStack();
    ~CommandStack();
    static CommandStack *instance;
};

/// Dispatching Commands. Callable from the UI thread and from background
/// workers (Twiki edit tools): the check-and-claim of the per-frame slot is
/// atomic, so two threads can never both queue for the same frame.
template <typename CommandClass, typename... ArgTypes> void ExecuteAfterDraw(ArgTypes... arguments) {
    CommandStack &commandStack = CommandStack::GetInstance();
    Command *command = new CommandClass(arguments...);
    if (!commandStack.TrySetNextCommand(command)) {
        delete command; // slot already taken this frame
    }
}

// Note: the QueueUndo / QueueRedo / QueueOnUIThread helpers are declared in
// Commands.h so callers can use them without seeing the ExecuteAfterDraw
// template body (which forces local instantiation of incomplete command
// types). Definitions live in CommandStack.cpp.
