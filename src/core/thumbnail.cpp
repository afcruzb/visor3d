#include "thumbnail.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "camera.h"
#include "mapped_file.h"
#include "shading.h"
#include "xml_scan.h"
#include "zip.h"

namespace v3d {

namespace {

bool embeddedThumbnail(const std::string& path, std::vector<uint8_t>& png) {
    MappedFile file;
    if (!file.open(path, nullptr)) return false;
    ZipArchive zip;
    std::string err;
    if (!zip.open(file.data(), file.size(), err)) return false;

    std::vector<std::string> candidates = {"Metadata/plate_1.png"};
    // OPC package thumbnail relationship, usually /Metadata/thumbnail.png.
    if (const ZipArchive::Entry* rels = zip.find("_rels/.rels")) {
        Buf<char> buf;
        if (zip.extract(*rels, buf, err)) {
            XmlScanner sc(buf.data(), buf.data() + buf.size());
            XmlTag t;
            while (sc.next(t))
                if (!t.closing && t.name == "Relationship" && xmlAttr(t, "Type").ends_with("/thumbnail"))
                    candidates.emplace_back(xmlAttr(t, "Target"));
        }
    }
    candidates.emplace_back("Metadata/thumbnail.png");

    for (const std::string& name : candidates) {
        const ZipArchive::Entry* e = zip.find(name);
        if (!e || e->size < 8 || e->size > (32u << 20)) continue;
        Buf<char> buf;
        if (!zip.extract(*e, buf, err)) continue;
        if (std::memcmp(buf.data(), "\x89PNG", 4) != 0) continue;
        png.assign(buf.begin(), buf.end());
        return true;
    }
    return false;
}

constexpr int kSuper = 2;  // supersampling factor per axis

struct V {
    float x, y, z;
};

struct Target {
    int w, h;
    std::vector<float> depth;
    std::vector<float> color;  // linear RGB
    std::vector<uint8_t> hit;
};

// Rasterises one triangle with per-vertex linear colours (flat = all equal).
void rasterTri(Target& t, const V s[3], const float c[3][3]) {
    float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[1].y - s[0].y) * (s[2].x - s[0].x);
    if (std::fabs(area) < 1e-12f) return;
    float inv = 1.f / area;
    int x0 = std::max(0, int(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
    int x1 = std::min(t.w - 1, int(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
    int y0 = std::max(0, int(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
    int y1 = std::min(t.h - 1, int(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
    for (int y = y0; y <= y1; ++y) {
        float py = y + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            float px = x + 0.5f;
            float w0 = ((s[1].x - px) * (s[2].y - py) - (s[1].y - py) * (s[2].x - px)) * inv;
            float w1 = ((s[2].x - px) * (s[0].y - py) - (s[2].y - py) * (s[0].x - px)) * inv;
            float w2 = 1.f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * s[0].z + w1 * s[1].z + w2 * s[2].z;
            size_t i = size_t(y) * size_t(t.w) + size_t(x);
            if (t.hit[i] && z <= t.depth[i]) continue;  // larger z = closer
            t.depth[i] = z;
            t.hit[i] = 1;
            for (int k = 0; k < 3; ++k) t.color[i * 3 + k] = w0 * c[0][k] + w1 * c[1][k] + w2 * c[2][k];
        }
    }
}

std::vector<uint8_t> renderThumbnail(const Scene& scene, int width, int height) {
    std::vector<uint8_t> out(size_t(width) * size_t(height) * 4, 0);
    if (width <= 0 || height <= 0 || scene.instances.empty()) return out;

    // Same orientation as the viewer opens with (a default Camera is isometric).
    float r[3], u[3], b[3];
    Camera().basis(r, u, b);
    auto toView = [&](const Mat4& m, const float* p, V& v) {
        float wx = m.m[0] * p[0] + m.m[4] * p[1] + m.m[8] * p[2] + m.m[12];
        float wy = m.m[1] * p[0] + m.m[5] * p[1] + m.m[9] * p[2] + m.m[13];
        float wz = m.m[2] * p[0] + m.m[6] * p[1] + m.m[10] * p[2] + m.m[14];
        v = {r[0] * wx + r[1] * wy + r[2] * wz, u[0] * wx + u[1] * wy + u[2] * wz, b[0] * wx + b[1] * wy + b[2] * wz};
    };

    // Pass 1: exact screen-space extent, so the model fills the thumbnail.
    float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
    for (const Instance& in : scene.instances) {
        const Mesh& m = scene.meshes[in.mesh];
        for (size_t i = 0; i < m.vertexCount(); ++i) {
            V v;
            toView(in.transform, &m.positions[i * 3], v);
            mn[0] = std::min(mn[0], v.x), mx[0] = std::max(mx[0], v.x);
            mn[1] = std::min(mn[1], v.y), mx[1] = std::max(mx[1], v.y);
        }
    }
    if (!(mx[0] >= mn[0]) || !(mx[1] >= mn[1])) return out;

    Target t{width * kSuper, height * kSuper, {}, {}, {}};
    t.depth.resize(size_t(t.w) * size_t(t.h));
    t.color.resize(t.depth.size() * 3);
    t.hit.assign(t.depth.size(), 0);
    const float margin = 0.04f;
    float sx = (mx[0] - mn[0]) > 0 ? t.w * (1 - 2 * margin) / (mx[0] - mn[0]) : 1e30f;
    float sy = (mx[1] - mn[1]) > 0 ? t.h * (1 - 2 * margin) / (mx[1] - mn[1]) : 1e30f;
    float scale = std::min(sx, sy);
    if (scale > 1e29f) scale = 1;
    float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]);

    // Orthographic: the viewer is along +Z in view space for every pixel.
    const float toViewer[3] = {0, 0, 1};
    const float worldUp[3] = {r[2], u[2], b[2]};  // world +Z in view space
    const shading::Light light(toViewer, worldUp);

    std::vector<float> palette(scene.palette.size() * 3);
    for (size_t i = 0; i < scene.palette.size(); ++i)
        for (int k = 0; k < 3; ++k)
            palette[i * 3 + k] = shading::toLinear(float(scene.palette[i] >> (8 * k) & 255) / 255.f);

    // Pass 2: transform each instance into a scratch buffer and rasterise.
    std::vector<V> view;
    std::vector<float> vnormal;
    for (const Instance& in : scene.instances) {
        const Mesh& m = scene.meshes[in.mesh];
        size_t nv = m.vertexCount();
        view.resize(nv);
        for (size_t i = 0; i < nv; ++i) {
            V v;
            toView(in.transform, &m.positions[i * 3], v);
            view[i] = {(v.x - cx) * scale + t.w * 0.5f, t.h * 0.5f - (v.y - cy) * scale, v.z};
        }
        bool smooth = m.normals.size() == m.positions.size();
        if (smooth) {
            // Normals: rotate by the instance transform (no scale handling needed:
            // re-normalised below) and into view space.
            vnormal.resize(nv * 3);
            const float* tm = in.transform.m;
            for (size_t i = 0; i < nv; ++i) {
                const float* n = &m.normals[i * 3];
                float wx = tm[0] * n[0] + tm[4] * n[1] + tm[8] * n[2];
                float wy = tm[1] * n[0] + tm[5] * n[1] + tm[9] * n[2];
                float wz = tm[2] * n[0] + tm[6] * n[1] + tm[10] * n[2];
                float vx = r[0] * wx + r[1] * wy + r[2] * wz, vy = u[0] * wx + u[1] * wy + u[2] * wz,
                      vz = b[0] * wx + b[1] * wy + b[2] * wz;
                float len = std::sqrt(vx * vx + vy * vy + vz * vz);
                float k = len > 0 ? 1.f / len : 0.f;
                vnormal[i * 3] = vx * k, vnormal[i * 3 + 1] = vy * k, vnormal[i * 3 + 2] = vz * k;
            }
        }
        const float* instColor = &palette[size_t(in.color) * 3];
        size_t nt = m.triangleCount();
        for (size_t tri = 0; tri < nt; ++tri) {
            uint32_t id[3];
            if (m.indices.empty()) id[0] = uint32_t(tri * 3), id[1] = id[0] + 1, id[2] = id[0] + 2;
            else std::memcpy(id, &m.indices[tri * 3], sizeof id);
            V s[3] = {view[id[0]], view[id[1]], view[id[2]]};
            const float* base = instColor;
            if (!m.triColor.empty() && m.triColor[tri]) base = &palette[size_t(m.triColor[tri]) * 3];
            float c[3][3];
            if (smooth) {
                for (int k = 0; k < 3; ++k) light.shade(&vnormal[size_t(id[k]) * 3], base, c[k]);
            } else {
                // Face normal from screen-space positions (y flipped back to up).
                float e1[3] = {s[1].x - s[0].x, -(s[1].y - s[0].y), (s[1].z - s[0].z) * scale};
                float e2[3] = {s[2].x - s[0].x, -(s[2].y - s[0].y), (s[2].z - s[0].z) * scale};
                float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (!(len > 0)) continue;
                for (float& x : n) x /= len;
                light.shade(n, base, c[0]);
                std::memcpy(c[1], c[0], sizeof c[0]);
                std::memcpy(c[2], c[0], sizeof c[0]);
            }
            rasterTri(t, s, c);
        }
    }

    // Box-filter down to the output size; alpha = sample coverage.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float acc[3] = {0, 0, 0};
            int covered = 0;
            for (int sy2 = 0; sy2 < kSuper; ++sy2)
                for (int sx2 = 0; sx2 < kSuper; ++sx2) {
                    size_t i = size_t(y * kSuper + sy2) * size_t(t.w) + size_t(x * kSuper + sx2);
                    if (!t.hit[i]) continue;
                    ++covered;
                    for (int k = 0; k < 3; ++k) acc[k] += t.color[i * 3 + k];
                }
            if (!covered) continue;
            uint8_t* px = &out[(size_t(y) * size_t(width) + size_t(x)) * 4];
            for (int k = 0; k < 3; ++k) px[k] = uint8_t(std::lround(shading::toDisplay(acc[k] / covered) * 255.f));
            px[3] = uint8_t(covered * 255 / (kSuper * kSuper));
        }
    return out;
}

}  // namespace

std::optional<Thumbnail> makeThumbnail(const std::string& path, int width, int height, const LoadOptions& options,
                                       std::string* error) {
    EncodedPng png;
    if (formatFromExtension(path) == Format::ThreeMF && embeddedThumbnail(path, png.bytes)) return png;

    Scene scene = loadScene(path, options);
    if (!scene.error.empty()) {
        if (error) *error = scene.error;
        return std::nullopt;
    }
    return RgbaImage{width, height, renderThumbnail(scene, width, height)};
}

}  // namespace v3d
