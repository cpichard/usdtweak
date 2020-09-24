#pragma once
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/camera.h> // TODO GET RID
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2d.h>
#ifdef _WIN32
#include <GL/glew.h>
#endif

//#define GLFW_INCLUDE_GLCOREARB
//#include <GLFW/glfw3.h>

PXR_NAMESPACE_USING_DIRECTIVE


typedef std::function<void(class TranslateGizmo&, GfCamera &, const GfVec2d&)> CaptureFunction;

class TranslateGizmo {

  public:
    TranslateGizmo();
    ~TranslateGizmo();

    // rename Hover ?? intersect ??
    bool Pick(const GfMatrix4d &mv, const GfMatrix4d &proj, const GfVec2d &clicked, const GfVec2d &sizes);

    ///
    bool CaptureEvents(GfCamera &camera, const GfVec2d &mousePosition);

    void StartEdition(GfCamera &camera, const GfVec2d &mousePosition);
    void Edit(GfCamera &camera, const GfVec2d &mousePosition);
    void EndEdition(GfCamera &camera, const GfVec2d &mousePosition);

    CaptureFunction _capture;

    void Draw(const GfMatrix4d &mv, const GfMatrix4d &proj);

  private:
    GfVec2d _limits;
    GfMatrix4d _mat;

    GfVec3f _origin;
    GfVec2d _mouseClickPosition;


    unsigned int axisGLBuffers;
    unsigned int vertexShader;
    unsigned int fragmentShader;
    unsigned int programShader;
    unsigned int vertexArrayObject;
    unsigned int modelViewUniform;
    unsigned int projectionUniform;
};
