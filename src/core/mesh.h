#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace v3d {

// Allocator that skips value-initialisation on resize(): large buffers are
// always overwritten by the parsers, so zero-filling them first is wasted work.
template <class T>
struct DefaultInitAlloc : std::allocator<T> {
    template <class U>
    struct rebind { using other = DefaultInitAlloc<U>; };
    using std::allocator<T>::allocator;
    template <class U>
    void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <class U, class... Args>
    void construct(U* p, Args&&... args) { ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...); }
};

template <class T>
using Buf = std::vector<T, DefaultInitAlloc<T>>;

struct BBox {
    float min[3] = {1e30f, 1e30f, 1e30f};
    float max[3] = {-1e30f, -1e30f, -1e30f};

    bool valid() const { return min[0] <= max[0] && min[1] <= max[1] && min[2] <= max[2]; }
    void add(float x, float y, float z);
    void add(const BBox& o);
};

// Column-major 4x4 matrix, same memory layout as OpenGL.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    bool isIdentity() const;
    static Mat4 mul(const Mat4& a, const Mat4& b);
};

// Default model colour when a file carries no colour information.
constexpr uint32_t kDefaultColor = 0xFFD8D2CDu;

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

struct Mesh {
    Buf<float> positions;      // xyz triplets
    Buf<float> normals;        // optional, per vertex (STEP)
    Buf<uint32_t> indices;     // optional; empty means triangle soup
    // Optional, one entry per triangle: 0 inherits the instance colour,
    // anything else is a palette index (MMU paint, CAD face colours).
    Buf<uint16_t> triColor;

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.empty() ? vertexCount() / 3 : indices.size() / 3; }
};

struct Instance {
    uint32_t mesh = 0;
    uint16_t color = 0;  // palette index
    Mat4 transform;
};

struct Scene {
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    // 0xAABBGGRR (RGBA8 little-endian). Entry 0 is always kDefaultColor.
    // loadScene() guarantees every Instance::color and triColor indexes it.
    std::vector<uint32_t> palette{kDefaultColor};
    BBox bounds;
    std::string error;

    // Appends a colour and returns its index. Past the 16-bit index space the
    // colour is dropped and the default (0) is returned. Inline: the STEP
    // plugin uses it without linking the core.
    uint16_t addColor(uint32_t color) {
        if (palette.size() > 0xFFFF) return 0;
        palette.push_back(color);
        return uint16_t(palette.size() - 1);
    }
    uint64_t triangleCount() const;
    void computeBounds();
};

}  // namespace v3d
