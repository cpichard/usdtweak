#pragma once

class Viewport;

/// Editing state -> this could be embedded in the manipulator ???
/// TODO: the name of the base class for a manipulator/editor/gizmo is confusing, find a better name
///        also it needs a bit of documentation now
struct ViewportEditor {
    virtual ~ViewportEditor() {};
    virtual void OnBeginEdition(Viewport &) {}; // Enter State
    virtual void OnEndEdition(Viewport &) {};  // Exit State
    virtual ViewportEditor *  OnUpdate(Viewport &) = 0;

    virtual bool IsMouseOver(const Viewport &) { return false; };

    /// Draw the translate manipulator as seen in the viewport
    virtual void OnDrawFrame(const Viewport &) {};

};