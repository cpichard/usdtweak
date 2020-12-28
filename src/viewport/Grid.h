#pragma once

class Viewport;

class Grid {
  public:
    Grid();
    void Render(Viewport &);

    /// Set if the up vector is Z, this will decide which plane to render (xy) or (xz)
    void SetZIsUp(bool);

  private:
    bool CompileShaders();

    unsigned int _vertexArrayObject;
    unsigned int _gridBuffer;
    unsigned int _programShader;
    unsigned int _modelViewUniform;
    unsigned int _projectionUniform;
    unsigned int _planeOrientation;
    float _zIsUp;
};