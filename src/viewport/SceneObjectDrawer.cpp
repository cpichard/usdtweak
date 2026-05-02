#include <pxr/imaging/garch/glApi.h>
#include "SceneObjectDrawer.h"
#include "Viewport.h"
#include "GlslCode.h"
#include "Constants.h"
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <iostream>
#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

static constexpr int kCircleSegments = 16;

SceneObjectDrawer::SceneObjectDrawer() {
    glGenVertexArrays(1, &_vertexArrayObject);
    glBindVertexArray(_vertexArrayObject);

    if (CompileShaders()) {
        _modelViewUniform  = glGetUniformLocation(_programShader, "modelView");
        _projectionUniform = glGetUniformLocation(_programShader, "projection");
        _colorUniform      = glGetUniformLocation(_programShader, "color");

        glGenBuffers(1, &_vertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);

        GLint posAttr = glGetAttribLocation(_programShader, "aPos");
        glVertexAttribPointer(posAttr, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(posAttr);
    } else {
        exit(ERROR_UNABLE_TO_COMPILE_SHADER);
    }
#ifndef __APPLE__
    glBindVertexArray(0);
#endif
}

bool SceneObjectDrawer::CompileShaders() {
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &SceneObjectVert, nullptr);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &SceneObjectFrag, nullptr);

    int success = 0;
    constexpr size_t logSize = 512;
    char logStr[logSize];

    glCompileShader(vertexShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, logSize, nullptr, logStr);
        std::cerr << "SceneObjectVert compilation failed\n" << logStr << std::endl;
        return false;
    }
    glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, logSize, nullptr, logStr);
        std::cerr << "SceneObjectFrag compilation failed\n" << logStr << std::endl;
        return false;
    }

    _programShader = glCreateProgram();
    glAttachShader(_programShader, vertexShader);
    glAttachShader(_programShader, fragmentShader);
    glBindAttribLocation(_programShader, 0, "aPos");
    glLinkProgram(_programShader);
    glGetProgramiv(_programShader, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(_programShader, logSize, nullptr, logStr);
        std::cerr << "SceneObjectDrawer program linking failed\n" << logStr << std::endl;
        return false;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return true;
}

void SceneObjectDrawer::BeginGeometry() {
    _positions.clear();
}

void SceneObjectDrawer::AddLine(const GfVec3d &a, const GfVec3d &b) {
    _positions.push_back((float)a[0]);
    _positions.push_back((float)a[1]);
    _positions.push_back((float)a[2]);
    _positions.push_back((float)b[0]);
    _positions.push_back((float)b[1]);
    _positions.push_back((float)b[2]);
}

void SceneObjectDrawer::AddCircle(const GfVec3d &center, const GfVec3d &axisA,
                                   const GfVec3d &axisB, double radius) {
    const double step = 2.0 * M_PI / kCircleSegments;
    GfVec3d prev = center + radius * axisA;
    for (int i = 1; i <= kCircleSegments; i++) {
        const double angle = i * step;
        GfVec3d next = center + radius * (std::cos(angle) * axisA + std::sin(angle) * axisB);
        AddLine(prev, next);
        prev = next;
    }
}

void SceneObjectDrawer::FlushGeometry(const GfMatrix4f &mv, const GfMatrix4f &proj,
                                       float r, float g, float b) {
    if (_positions.empty()) return;

    glBindVertexArray(_vertexArrayObject);
    glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, _positions.size() * sizeof(float),
                 _positions.data(), GL_STREAM_DRAW);
    glUniformMatrix4fv(_modelViewUniform,  1, GL_FALSE, mv.data());
    glUniformMatrix4fv(_projectionUniform, 1, GL_FALSE, proj.data());
    glUniform4f(_colorUniform, r, g, b, 1.0f);
    glDrawArrays(GL_LINES, 0, (GLsizei)(_positions.size() / 3));
    _positions.clear();
}

void SceneObjectDrawer::AddCameraFrustum(const UsdPrim &prim, const UsdTimeCode &tc) {
    GfFrustum frustum = UsdGeomCamera(prim).GetCamera(tc).GetFrustum();
    const std::vector<GfVec3d> c = frustum.ComputeCorners();
    if (c.size() < 8) return;

    // Near quad (corners 0-3: LB, RB, LT, RT)
    AddLine(c[0], c[1]);
    AddLine(c[1], c[3]);
    AddLine(c[3], c[2]);
    AddLine(c[2], c[0]);
    // Far quad (corners 4-7)
    AddLine(c[4], c[5]);
    AddLine(c[5], c[7]);
    AddLine(c[7], c[6]);
    AddLine(c[6], c[4]);
    // Side edges
    AddLine(c[0], c[4]);
    AddLine(c[1], c[5]);
    AddLine(c[2], c[6]);
    AddLine(c[3], c[7]);

    // Small cross at camera position
    const GfVec3d pos = frustum.GetPosition();
    const double s = std::max(frustum.GetNearFar().GetMin() * 0.2, 1e-6);
    AddLine(pos - GfVec3d(s, 0, 0), pos + GfVec3d(s, 0, 0));
    AddLine(pos - GfVec3d(0, s, 0), pos + GfVec3d(0, s, 0));
    AddLine(pos - GfVec3d(0, 0, s), pos + GfVec3d(0, 0, s));
}

void SceneObjectDrawer::AddLightIcon(const Viewport &viewport, const UsdPrim &prim,
                                      const UsdTimeCode &tc) {
    GfMatrix4d xf = UsdGeomImageable(prim).ComputeLocalToWorldTransform(tc);
    const GfVec3d pos = xf.ExtractTranslation();

    GfVec3d xAxis(xf[0][0], xf[0][1], xf[0][2]);
    GfVec3d yAxis(xf[1][0], xf[1][1], xf[1][2]);
    GfVec3d zAxis(xf[2][0], xf[2][1], xf[2][2]);
    const double xLen = xAxis.GetLength();
    const double yLen = yAxis.GetLength();
    const double zLen = zAxis.GetLength();
    if (xLen > 1e-9) xAxis /= xLen;
    if (yLen > 1e-9) yAxis /= yLen;
    if (zLen > 1e-9) zAxis /= zLen;

    const double s = viewport.ComputeScaleFactor(pos);

    // Universal 3-axis cross at the light position
    AddLine(pos - s * xAxis, pos + s * xAxis);
    AddLine(pos - s * yAxis, pos + s * yAxis);
    AddLine(pos - s * zAxis, pos + s * zAxis);

    // Type-specific shapes
    if (prim.IsA<UsdLuxRectLight>()) {
        UsdLuxRectLight rectLight(prim);
        float w = 1.0f, h = 1.0f;
        rectLight.GetWidthAttr().Get(&w, tc);
        rectLight.GetHeightAttr().Get(&h, tc);
        const GfVec3d bl = pos - (w * 0.5) * xAxis - (h * 0.5) * yAxis;
        const GfVec3d br = pos + (w * 0.5) * xAxis - (h * 0.5) * yAxis;
        const GfVec3d tr = pos + (w * 0.5) * xAxis + (h * 0.5) * yAxis;
        const GfVec3d tl = pos - (w * 0.5) * xAxis + (h * 0.5) * yAxis;
        AddLine(bl, br); AddLine(br, tr); AddLine(tr, tl); AddLine(tl, bl);
        // Emission direction indicator
        AddLine(pos, pos - s * 1.5 * zAxis);
    } else if (prim.IsA<UsdLuxDiskLight>()) {
        UsdLuxDiskLight diskLight(prim);
        float r = 0.5f;
        diskLight.GetRadiusAttr().Get(&r, tc);
        AddCircle(pos, xAxis, yAxis, r);
        AddLine(pos, pos - s * 1.5 * zAxis);
    } else if (prim.IsA<UsdLuxSphereLight>()) {
        UsdLuxSphereLight sphereLight(prim);
        float r = 0.5f;
        sphereLight.GetRadiusAttr().Get(&r, tc);
        if (r < 1e-6f) r = (float)s;
        AddCircle(pos, xAxis, yAxis, r);
        AddCircle(pos, yAxis, zAxis, r);
        AddCircle(pos, zAxis, xAxis, r);
    } else if (prim.IsA<UsdLuxDistantLight>()) {
        // 3 parallel rays along -Z (light travels in -Z direction)
        const double len = s * 3.0;
        for (int i = -1; i <= 1; i++) {
            const GfVec3d origin = pos + (i * s * 0.6) * xAxis;
            const GfVec3d tip = origin - len * zAxis;
            AddLine(origin, tip);
            AddLine(tip, tip + s * 0.4 * (zAxis + 0.3 * xAxis));
            AddLine(tip, tip + s * 0.4 * (zAxis - 0.3 * xAxis));
        }
    } else if (prim.IsA<UsdLuxCylinderLight>()) {
        UsdLuxCylinderLight cylLight(prim);
        float r = 0.5f, length = 1.0f;
        cylLight.GetRadiusAttr().Get(&r, tc);
        cylLight.GetLengthAttr().Get(&length, tc);
        const double halfLen = length * 0.5;
        const GfVec3d endA = pos - halfLen * xAxis;
        const GfVec3d endB = pos + halfLen * xAxis;
        AddCircle(endA, yAxis, zAxis, r);
        AddCircle(endB, yAxis, zAxis, r);
        AddLine(endA + r * yAxis, endB + r * yAxis);
        AddLine(endA - r * yAxis, endB - r * yAxis);
        AddLine(endA + r * zAxis, endB + r * zAxis);
        AddLine(endA - r * zAxis, endB - r * zAxis);
    }
}

void SceneObjectDrawer::Render(const Viewport &viewport) {
    const auto stage = viewport.GetCurrentStage();
    if (!stage) return;

    const ImagingSettings &settings = viewport.GetImagingSettings();
    if (!settings.showCameras && !settings.showLights) return;

    const UsdTimeCode tc = viewport.GetCurrentTimeCode();
    const GfCamera cam = viewport.GetViewportCamera();
    const GfMatrix4f mv(cam.GetFrustum().ComputeViewMatrix());
    const GfMatrix4f proj(cam.GetFrustum().ComputeProjectionMatrix());

    glUseProgram(_programShader);
    glEnable(GL_DEPTH_TEST);

    if (settings.showCameras) {
        BeginGeometry();
        for (const auto &prim : stage->Traverse()) {
            if (prim.IsA<UsdGeomCamera>()) {
                AddCameraFrustum(prim, tc);
            }
        }
        FlushGeometry(mv, proj, 0.0f, 0.8f, 0.8f);
    }

    if (settings.showLights) {
        BeginGeometry();
        for (const auto &prim : stage->Traverse()) {
            if (prim.HasAPI<UsdLuxLightAPI>()) {
                AddLightIcon(viewport, prim, tc);
            }
        }
        FlushGeometry(mv, proj, 1.0f, 0.85f, 0.0f);
    }

    glUseProgram(0);
#ifndef __APPLE__
    glBindVertexArray(0);
#endif
}
