#pragma once
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/camera.h> // TODO GET RID
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/usd/usdGeom/gprim.h>

#ifdef _WIN32
#include <GL/glew.h>
#endif


PXR_NAMESPACE_USING_DIRECTIVE
struct ViewportEditor;
class Viewport;

// A Manipulator can be seen in multiple viewport
// but only edited in one

class TranslateManipulator final {

  public:
    TranslateManipulator();
    ~TranslateManipulator();

    /// Return true if the mouse is over this manipulator for the viewport passed in argument
    bool IsMouseOver(const Viewport &);

    /// Process the events and action what has to be manipulated
    void OnProcessFrameEvents(Viewport &viewport);

    /// Draw the translate manipulator as seen in the viewport
    void OnDrawFrame(const Viewport &);

    /// Called when the viewport changes its selection
    void OnSelectionChange(Viewport &);

    /// Return a new editing state for the viewport. The editing state is in charge of controlling the
    /// translate manipulator
    ViewportEditor * NewEditingState(Viewport &viewport);

    typedef enum { // use class enum ??
        XAxis,
        YAxis,
        ZAxis,
        None,
    } SelectedAxis;

private:
    SelectedAxis _selectedAxis;
    UsdGeomXformable _xformable;
    UsdGeomXformOp _translateOp;

    GfMatrix4d _mat;

    GfVec3f _origin;
    GfVec2d _mouseClickPosition;


    // OpenGL stuff
    unsigned int axisGLBuffers;
    unsigned int vertexShader;
    unsigned int fragmentShader;
    unsigned int programShader;
    unsigned int vertexArrayObject;
    unsigned int modelViewUniform;
    unsigned int projectionUniform;
    unsigned int originUniform;
};
