#pragma once
#include <memory>
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
    friend class SdfUndoRedoRecorder;
    
    static CommandStack &GetInstance();

    inline bool HasNextCommand() { return lastCmd != nullptr; }
    inline void SetNextCommand(Command *command) { lastCmd = command; }
    
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

    /// The ProcessCommands function is called after the frame is rendered and displayed and execute the
    /// last command. The command passed here now belongs to this stack
    void _PushCommand(Command *cmd);

  private:
    CommandStack();
    ~CommandStack();
    static CommandStack *instance;
};

/// Dispatching Commands.
template <typename CommandClass, typename... ArgTypes> void ExecuteAfterDraw(ArgTypes... arguments) {
    CommandStack &commandStack = CommandStack::GetInstance();
    if (!commandStack.HasNextCommand()) {
        commandStack.SetNextCommand(new CommandClass(arguments...));
    }
}
