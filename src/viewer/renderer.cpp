#include "renderer.h"

#include <algorithm>
#include <cmath>

#include "../core/shading.h"

namespace v3d {

namespace {

const char* kModelVs = R"(layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uModelView;
uniform mat4 uProj;
uniform mat3 uNormalMat;
out vec3 vPos;
out vec3 vNormal;
void main() {
    vec4 p = uModelView * vec4(aPos, 1.0);
    vPos = p.xyz;
    vNormal = uNormalMat * aNormal;
    gl_Position = uProj * p;
}
)";

// Flat shading uses screen-space derivatives, so STL/3MF need no normal buffer.
// Lighting constants come from core/shading.h (shared with the thumbnailer).
const char* kModelFs = R"(in vec3 vPos;
in vec3 vNormal;
uniform vec4 uColor;
uniform int uSmooth;
uniform int uTriColors;
uniform usamplerBuffer uTriColor;
uniform samplerBuffer uPalette;
uniform vec3 uUp;
uniform vec4 uOverlay;
out vec4 fragColor;
void main() {
    if (uOverlay.a > 0.0) { fragColor = uOverlay; return; }
    vec3 base = uColor.rgb;
    if (uTriColors != 0) {
        // 0 = unpainted: keep the part colour.
        uint idx = texelFetch(uTriColor, gl_PrimitiveID).r;
        if (idx != 0u) base = texelFetch(uPalette, int(idx)).rgb;
    }
    vec3 n = uSmooth != 0 ? normalize(vNormal) : normalize(cross(dFdx(vPos), dFdy(vPos)));
    vec3 v = normalize(-vPos);
    if (dot(n, v) < 0.0) n = -n;
    vec3 l = normalize(v + LIGHT_OFFSET);
    float diff = max(dot(n, l), 0.0);
    float hemi = dot(n, uUp) * 0.5 + 0.5;
    vec3 amb = mix(GROUND, SKY, hemi);
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), SPEC_POWER) * SPEC_STRENGTH;
    vec3 c = pow(base, vec3(GAMMA)) * (amb + DIFFUSE * diff) + spec;
    fragColor = vec4(pow(c, vec3(1.0 / GAMMA)), 1.0);
}
)";

const char* kGridVs = R"(layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

const char* kGridFs = R"(uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

// Full-screen triangle from gl_VertexID, vertical gradient.
const char* kBgVs = R"(out float vY;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    vY = p.y * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kBgFs = R"(
in float vY;
out vec4 fragColor;
void main() { fragColor = vec4(mix(vec3(0.11, 0.12, 0.135), vec3(0.30, 0.32, 0.36), vY), 1.0); }
)";

GLuint compile(GLenum type, const char* body, std::string& error) {
    static const std::string prelude = "#version 330 core\n" + shading::glslDefines();
    const char* parts[] = {prelude.c_str(), body};
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, parts, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const char* vs, const char* fs, std::string& error) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, error);
    if (!v) return 0;
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, error);
    if (!f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        error = log;
        return 0;
    }
    return p;
}

// Cofactor matrix of the upper-left 3x3 = det * inverse-transpose. The shader
// normalises, and two-sided lighting hides the sign flip of mirrored transforms.
void normalMatrix(const Mat4& mv, float out[9]) {
    const float* m = mv.m;
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], h = m[6], i = m[10];
    // Column-major: out[col * 3 + row] = cofactor(row, col).
    out[0] = e * i - f * h, out[3] = -(d * i - f * g), out[6] = d * h - e * g;
    out[1] = -(b * i - c * h), out[4] = a * i - c * g, out[7] = -(a * h - b * g);
    out[2] = b * f - c * e, out[5] = -(a * f - c * d), out[8] = a * e - b * d;
}

}  // namespace

bool Renderer::init(std::string& error) {
    if (!(modelProg_ = link(kModelVs, kModelFs, error))) return false;
    if (!(gridProg_ = link(kGridVs, kGridFs, error))) return false;
    if (!(bgProg_ = link(kBgVs, kBgFs, error))) return false;

    u_.modelView = glGetUniformLocation(modelProg_, "uModelView");
    u_.proj = glGetUniformLocation(modelProg_, "uProj");
    u_.normalMat = glGetUniformLocation(modelProg_, "uNormalMat");
    u_.color = glGetUniformLocation(modelProg_, "uColor");
    u_.smooth = glGetUniformLocation(modelProg_, "uSmooth");
    u_.triColors = glGetUniformLocation(modelProg_, "uTriColors");
    u_.up = glGetUniformLocation(modelProg_, "uUp");
    u_.overlay = glGetUniformLocation(modelProg_, "uOverlay");
    glUseProgram(modelProg_);
    glUniform1i(glGetUniformLocation(modelProg_, "uTriColor"), 0);
    glUniform1i(glGetUniformLocation(modelProg_, "uPalette"), 1);
    gridViewProj_ = glGetUniformLocation(gridProg_, "uViewProj");
    gridColor_ = glGetUniformLocation(gridProg_, "uColor");

    glGenVertexArrays(1, &emptyVao_);
    glGenVertexArrays(1, &gridVao_);
    glGenBuffers(1, &gridVbo_);
    glBindVertexArray(gridVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenBuffers(1, &paletteBuf_);
    glGenTextures(1, &paletteTex_);
    return true;
}

void Renderer::clearScene() {
    for (GpuMesh& m : meshes_) {
        GLuint bufs[] = {m.vbo, m.nbo, m.ebo, m.colorBuf};
        glDeleteBuffers(4, bufs);
        glDeleteVertexArrays(1, &m.vao);
        if (m.colorTex) glDeleteTextures(1, &m.colorTex);
    }
    meshes_.clear();
    instances_.clear();
    gridMinor_ = gridMajor_ = 0;
}

void Renderer::setScene(Scene& scene) {
    clearScene();
    meshes_.reserve(scene.meshes.size());
    for (Mesh& src : scene.meshes) {
        GpuMesh g;
        glGenVertexArrays(1, &g.vao);
        glBindVertexArray(g.vao);

        glGenBuffers(1, &g.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(src.positions.size() * sizeof(float)), src.positions.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        if (!src.normals.empty()) {
            glGenBuffers(1, &g.nbo);
            glBindBuffer(GL_ARRAY_BUFFER, g.nbo);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(src.normals.size() * sizeof(float)), src.normals.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
            g.normals = true;
        }
        if (!src.indices.empty()) {
            glGenBuffers(1, &g.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(src.indices.size() * sizeof(uint32_t)), src.indices.data(), GL_STATIC_DRAW);
            g.indexed = true;
            g.count = GLsizei(src.indices.size());
        } else {
            g.count = GLsizei(src.vertexCount());
        }
        if (!src.triColor.empty()) {
            glGenBuffers(1, &g.colorBuf);
            glBindBuffer(GL_TEXTURE_BUFFER, g.colorBuf);
            glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(src.triColor.size() * sizeof(uint16_t)), src.triColor.data(), GL_STATIC_DRAW);
            glGenTextures(1, &g.colorTex);
            glBindTexture(GL_TEXTURE_BUFFER, g.colorTex);
            glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, g.colorBuf);
            g.triColors = true;
        }
        meshes_.push_back(g);
        src = Mesh{};  // release CPU copy
    }
    glBindVertexArray(0);

    glBindBuffer(GL_TEXTURE_BUFFER, paletteBuf_);
    glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(scene.palette.size() * sizeof(uint32_t)), scene.palette.data(), GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, paletteTex_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, paletteBuf_);

    instances_.reserve(scene.instances.size());
    for (const Instance& in : scene.instances) {
        uint32_t c = scene.palette[std::min<size_t>(in.color, scene.palette.size() - 1)];
        instances_.push_back({in.mesh, in.transform,
                              {(c & 255) / 255.f, (c >> 8 & 255) / 255.f, (c >> 16 & 255) / 255.f, 1.f}});
    }
    buildGrid(scene.bounds);
}

void Renderer::buildGrid(const BBox& b) {
    float ext = std::max(b.max[0] - b.min[0], b.max[1] - b.min[1]);
    if (!(ext > 0)) ext = std::max(b.max[2] - b.min[2], 1.0f);
    float step = std::pow(10.0f, std::floor(std::log10(ext))) / 10.0f;
    float major = step * 10.0f;
    float cx = 0.5f * (b.min[0] + b.max[0]), cy = 0.5f * (b.min[1] + b.max[1]);
    float half = std::ceil(ext * 0.65f / major) * major;
    float x0 = std::floor((cx - half) / major) * major, x1 = std::ceil((cx + half) / major) * major;
    float y0 = std::floor((cy - half) / major) * major, y1 = std::ceil((cy + half) / major) * major;
    float z = b.min[2] - ext * 1e-4f;  // just below the model, avoids z-fighting

    std::vector<float> minor, majorV;
    auto line = [](std::vector<float>& v, float ax, float ay, float bx, float by, float z) {
        v.insert(v.end(), {ax, ay, z, bx, by, z});
    };
    int nx = int(std::lround((x1 - x0) / step)), ny = int(std::lround((y1 - y0) / step));
    for (int i = 0; i <= nx; ++i) {
        float x = x0 + i * step;
        line(i % 10 == 0 ? majorV : minor, x, y0, x, y1, z);
    }
    for (int i = 0; i <= ny; ++i) {
        float y = y0 + i * step;
        line(i % 10 == 0 ? majorV : minor, x0, y, x1, y, z);
    }
    gridMinor_ = GLsizei(minor.size() / 3);
    gridMajor_ = GLsizei(majorV.size() / 3);
    minor.insert(minor.end(), majorV.begin(), majorV.end());
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(minor.size() * sizeof(float)), minor.data(), GL_STATIC_DRAW);
}

void Renderer::drawMeshes(const Mat4& view, bool overlay) {
    glUniform4f(u_.overlay, 0.f, 0.f, 0.f, overlay ? 0.45f : 0.f);
    for (const GpuInstance& in : instances_) {
        const GpuMesh& g = meshes_[in.mesh];
        Mat4 mv = Mat4::mul(view, in.model);
        float nm[9];
        normalMatrix(mv, nm);
        glUniformMatrix4fv(u_.modelView, 1, GL_FALSE, mv.m);
        glUniformMatrix3fv(u_.normalMat, 1, GL_FALSE, nm);
        glUniform4f(u_.color, in.color[0], in.color[1], in.color[2], 1.f);
        glUniform1i(u_.smooth, g.normals);
        glUniform1i(u_.triColors, g.triColors);
        if (g.triColors) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_BUFFER, g.colorTex);
        }
        glBindVertexArray(g.vao);
        if (g.indexed)
            glDrawElements(GL_TRIANGLES, g.count, GL_UNSIGNED_INT, nullptr);
        else
            glDrawArrays(GL_TRIANGLES, 0, g.count);
    }
}

bool Renderer::ensureMsaaTarget(int w, int h) {
    if (msaaFbo_ && msaaW_ == w && msaaH_ == h) return true;
    if (!msaaFbo_) {
        glGenFramebuffers(1, &msaaFbo_);
        glGenRenderbuffers(1, &msaaColor_);
        glGenRenderbuffers(1, &msaaDepth_);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, msaaColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msaaDepth_);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    msaaW_ = w, msaaH_ = h;
    if (!ok) msaa_ = false;  // fall back to aliased rendering
    return ok;
}

void Renderer::draw(const Camera& camera, int w, int h) {
    if (msaa_ && w > 0 && h > 0 && ensureMsaaTarget(w, h)) {
        glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo_);
        drawScene(camera, w, h);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    drawScene(camera, w, h);
}

void Renderer::drawScene(const Camera& camera, int w, int h) {
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(bgProg_);
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (instances_.empty()) return;

    float aspect = float(w) / float(std::max(h, 1));
    Mat4 view = camera.view();
    Mat4 proj = camera.projection(aspect);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glUseProgram(modelProg_);
    glUniformMatrix4fv(u_.proj, 1, GL_FALSE, proj.m);
    glUniform3f(u_.up, view.m[8], view.m[9], view.m[10]);  // world +Z in view space
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, paletteTex_);

    if (wireframe) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.f, 1.f);
    }
    drawMeshes(view, false);
    if (wireframe) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        drawMeshes(view, true);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_BLEND);
    }

    if (grid && gridMinor_ + gridMajor_ > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(gridProg_);
        Mat4 vp = Mat4::mul(proj, view);
        glUniformMatrix4fv(gridViewProj_, 1, GL_FALSE, vp.m);
        glBindVertexArray(gridVao_);
        glUniform4f(gridColor_, 1.f, 1.f, 1.f, 0.06f);
        glDrawArrays(GL_LINES, 0, gridMinor_);
        glUniform4f(gridColor_, 1.f, 1.f, 1.f, 0.16f);
        glDrawArrays(GL_LINES, gridMinor_, gridMajor_);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}

}  // namespace v3d
