#pragma once

class Viewport;

/// Base class for a manipulator.
/// The manipulator can be seen as an editing state part of a FSM
/// It also knows how to draw itself in the viewport

/// This base class forces the override of OnUpdate which is called every frame.
/// The OnUpdate should return the new manipulator to use after the update or itself
/// if the state (Translate/Rotate) should be the same

struct Manipulator {
    virtual ~Manipulator(){};
    virtual void OnBeginEdition(Viewport &){};     // Enter State
    virtual void OnEndEdition(Viewport &){};       // Exit State
    virtual Manipulator *OnUpdate(Viewport &) = 0; // Next State

    virtual bool IsMouseOver(const Viewport &) { return false; };

    /// Draw the translate manipulator as seen in the viewport
    virtual void OnDrawFrame(const Viewport &){};

    /// Called when the viewport changes its selection
    virtual void OnSelectionChange(Viewport &){};
    
    // Manipulator axis, for convenience as
    typedef enum {
        XAxis = 0,
        YAxis,
        ZAxis,
        None,
    } ManipulatorAxis;
};

