#pragma once

/// Editing state -> this could be embedded in the manipulator ???
struct ViewportEditor {
    virtual void OnEnter() {}; // OnBeginEdition() // Enter State
    virtual void OnExit() {};  // OnEndEdition()   // Exit State
    // BeginModification(); // Store the first modification
    // EndModification();   // Store the last commands ?
    virtual ViewportEditor *  NextState() = 0; // OnUpdate()
};