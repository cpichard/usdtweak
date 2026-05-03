#pragma once
#include <vector>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>

PXR_NAMESPACE_USING_DIRECTIVE

class Viewport;

class SceneObjectDrawer {
  public:
    SceneObjectDrawer();
    void Render(const Viewport &viewport);

  private:
    bool CompileShaders();

    unsigned int _vertexArrayObject = 0;
    unsigned int _vertexBuffer = 0;
    unsigned int _programShader = 0;
    unsigned int _modelViewUniform = 0;
    unsigned int _projectionUniform = 0;
    unsigned int _colorUniform = 0;

    std::vector<float> _positions;

    void BeginGeometry();
    void AddLine(const GfVec3d &a, const GfVec3d &b);
    void AddCircle(const GfVec3d &center, const GfVec3d &axisA,
                   const GfVec3d &axisB, double radius);
    void FlushGeometry(const GfMatrix4f &mv, const GfMatrix4f &proj,
                       float r, float g, float b);
    void AddCameraFrustum(const UsdPrim &prim, const UsdTimeCode &tc);
    void AddLightIcon(const Viewport &viewport, const UsdPrim &prim,
                      const UsdTimeCode &tc);
};
