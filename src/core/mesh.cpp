#include "mesh.h"

#include <algorithm>
#include <cstring>

namespace v3d {

void BBox::add(float x, float y, float z) {
    min[0] = std::min(min[0], x);
    min[1] = std::min(min[1], y);
    min[2] = std::min(min[2], z);
    max[0] = std::max(max[0], x);
    max[1] = std::max(max[1], y);
    max[2] = std::max(max[2], z);
}

void BBox::add(const BBox& o) {
    if (!o.valid()) return;
    add(o.min[0], o.min[1], o.min[2]);
    add(o.max[0], o.max[1], o.max[2]);
}

bool Mat4::isIdentity() const {
    static const Mat4 id;
    return std::memcmp(m, id.m, sizeof m) == 0;
}

Mat4 Mat4::mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

uint64_t Scene::triangleCount() const {
    uint64_t n = 0;
    for (const Instance& inst : instances) n += meshes[inst.mesh].triangleCount();
    return n;
}

// Bounds of vertices, ignoring NaNs: std::min/max return the first argument
// when the comparison involves a NaN, and the loops stay branch-free.
static BBox meshBounds(const Mesh& mesh, const Mat4& t, bool identity) {
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    const float* p = mesh.positions.data();
    const size_t n = mesh.vertexCount();
    const float* m = t.m;
    for (size_t i = 0; i < n; ++i, p += 3) {
        float x = p[0], y = p[1], z = p[2];
        if (!identity) {
            float tx = m[0] * x + m[4] * y + m[8] * z + m[12];
            float ty = m[1] * x + m[5] * y + m[9] * z + m[13];
            float tz = m[2] * x + m[6] * y + m[10] * z + m[14];
            x = tx, y = ty, z = tz;
        }
        mn[0] = std::min(mn[0], x), mx[0] = std::max(mx[0], x);
        mn[1] = std::min(mn[1], y), mx[1] = std::max(mx[1], y);
        mn[2] = std::min(mn[2], z), mx[2] = std::max(mx[2], z);
    }
    BBox b;
    std::memcpy(b.min, mn, sizeof mn);
    std::memcpy(b.max, mx, sizeof mx);
    return b;
}

void Scene::computeBounds() {
    bounds = BBox{};
    for (const Instance& inst : instances)
        bounds.add(meshBounds(meshes[inst.mesh], inst.transform, inst.transform.isIdentity()));
}

}  // namespace v3d
