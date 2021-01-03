#pragma once

class Viewport;

/// Base class for a manipulator. The manipulator is multiple thing at the same time, it is seen as an editing state from a fsm
/// as a portion of code which renders a gizmo,

/// It forces to override the function OnUpdate which is called for every rendered frame.
/// The manipulator should return the

struct Manipulator {
    virtual ~Manipulator(){};
    virtual void OnBeginEdition(Viewport &){}; // Enter State
    virtual void OnEndEdition(Viewport &){};   // Exit State
    virtual Manipulator *OnUpdate(Viewport &) = 0; // Next State

    virtual bool IsMouseOver(const Viewport &) { return false; };

    /// Draw the translate manipulator as seen in the viewport
    virtual void OnDrawFrame(const Viewport &){};

    /// Called when the viewport changes its selection
    virtual void OnSelectionChange(Viewport &){};
};