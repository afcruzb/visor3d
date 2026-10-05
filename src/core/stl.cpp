#include <algorithm>
#include <cstring>

#include "formats.h"
#include "parallel.h"
#include "parse.h"

namespace v3d {

namespace {

constexpr size_t kHeader = 84;
constexpr size_t kRecord = 50;  // normal(12) + 3 vertices(36) + attribute(2)

void parseBinary(const uint8_t* data, size_t tris, Mesh& mesh) {
    mesh.positions.resize(tris * 9);
    float* dst = mesh.positions.data();
    const uint8_t* src = data + kHeader + 12;

    // Splitting only pays off once each chunk is a sizeable copy.
    constexpr size_t kPerChunk = 250000;
    const size_t chunks = std::clamp<size_t>(tris / kPerChunk, 1, hardwareThreads());
    parallelFor(chunks, [=](size_t c) {
        for (size_t i = tris * c / chunks, end = tris * (c + 1) / chunks; i < end; ++i)
            std::memcpy(dst + i * 9, src + i * kRecord, 36);
    });
}

bool parseAscii(const char* p, const char* end, Mesh& mesh, std::string& error) {
    mesh.positions.reserve(size_t(end - p) / 80 * 3);  // ~80 bytes per "vertex" line
    static constexpr char kVertex[] = "vertex";
    while (true) {
        const char* v = static_cast<const char*>(memmem(p, size_t(end - p), kVertex, 6));
        if (!v) break;
        p = v + 6;
        float x, y, z;
        // A bad number ends the mesh: truncated files keep their complete triangles.
        if (!(p = parseFloat(p, end, x)) || !(p = parseFloat(p, end, y)) || !(p = parseFloat(p, end, z))) break;
        mesh.positions.push_back(x);
        mesh.positions.push_back(y);
        mesh.positions.push_back(z);
    }
    // Drop a trailing incomplete triangle from truncated files.
    mesh.positions.resize(mesh.positions.size() / 9 * 9);
    if (mesh.positions.empty()) {
        error = "STL sin triángulos";
        return false;
    }
    return true;
}

}  // namespace

void loadStl(const uint8_t* data, size_t size, Scene& scene) {
    Mesh mesh;
    const char* text = reinterpret_cast<const char*>(data);
    const char* textEnd = text + size;
    const char* first = skipSpace(text, textEnd);
    bool looksAscii = startsWith({first, size_t(textEnd - first)}, "solid");

    uint32_t tris = 0;
    if (size >= kHeader) std::memcpy(&tris, data + 80, 4);
    size_t expected = kHeader + size_t(tris) * kRecord;

    // Triangles actually present, for binary files cut short.
    size_t available = size > kHeader ? (size - kHeader) / kRecord : 0;
    size_t partial = std::min<size_t>(tris, available);

    // Binary files may also start with "solid", so an exact size match wins.
    // Binary with trailing garbage is accepted only when it does not look ASCII.
    if (size >= kHeader && (size == expected || (!looksAscii && size > expected))) {
        if (tris == 0) {
            scene.error = "STL sin triángulos";
            return;
        }
        parseBinary(data, tris, mesh);
    } else if (looksAscii && parseAscii(text, textEnd, mesh, scene.error)) {
        // ASCII it is.
    } else if (partial > 0) {
        // Truncated binary (possibly with a "solid..." header, as SolidWorks writes).
        scene.error.clear();
        parseBinary(data, partial, mesh);
    } else {
        if (scene.error.empty()) scene.error = "STL truncado o no válido";
        return;
    }

    scene.meshes.push_back(std::move(mesh));
    scene.instances.push_back(Instance{});
}

}  // namespace v3d
