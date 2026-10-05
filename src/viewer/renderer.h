#pragma once

#include <string>
#include <vector>

#include "../core/mesh.h"
#include "../core/camera.h"
#include "gl_loader.h"

namespace v3d {

class Renderer {
public:
    bool init(std::string& error);
    // Uploads the scene to the GPU and releases its CPU-side geometry.
    void setScene(Scene& scene);
    void clearScene();
    void draw(const Camera& camera, int fbWidth, int fbHeight);
    bool hasScene() const { return !instances_.empty(); }
    // 4x MSAA through an offscreen target. Off until enabled: allocating the
    // multisample buffers at HiDPI costs tens of ms, too much for frame one.
    void enableMsaa() { msaa_ = true; }

    bool wireframe = false;
    bool grid = true;

private:
    struct GpuMesh {
        GLuint vao = 0, vbo = 0, nbo = 0, ebo = 0, colorBuf = 0, colorTex = 0;
        GLsizei count = 0;
        bool indexed = false, normals = false, triColors = false;
    };
    struct GpuInstance {
        uint32_t mesh;
        Mat4 model;
        float color[4];
    };

    void buildGrid(const BBox& b);
    void drawScene(const Camera& camera, int fbWidth, int fbHeight);
    bool ensureMsaaTarget(int w, int h);
    void drawMeshes(const Mat4& view, bool overlay);

    std::vector<GpuMesh> meshes_;
    std::vector<GpuInstance> instances_;

    GLuint modelProg_ = 0, gridProg_ = 0, bgProg_ = 0;
    GLuint emptyVao_ = 0, gridVao_ = 0, gridVbo_ = 0;
    GLuint paletteBuf_ = 0, paletteTex_ = 0;
    GLsizei gridMinor_ = 0, gridMajor_ = 0;
    bool msaa_ = false;
    GLuint msaaFbo_ = 0, msaaColor_ = 0, msaaDepth_ = 0;
    int msaaW_ = 0, msaaH_ = 0;

    struct {
        GLint modelView, proj, normalMat, color, smooth, triColors, up, overlay;
    } u_{};
    GLint gridViewProj_ = -1, gridColor_ = -1;
};

}  // namespace v3d
