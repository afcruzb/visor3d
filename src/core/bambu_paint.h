#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace v3d {

// One leaf of a painted triangle: three vertices and the painted state
// (0 = not painted / inherit the part's filament, N = filament N).
struct PaintLeaf {
    float v[9];
    uint8_t state;
};

// Decodes a per-triangle "paint_color" (Bambu Studio, OrcaSlicer) or
// "slic3rpe:mmu_segmentation" (PrusaSlicer) attribute: a recursive subdivision
// tree serialised as hex nibbles, last nibble first. Appends the leaves of
// triangle (a, b, c) to `out`. Returns false on malformed data; `out` is then
// left as it was.
bool decodePaint(std::string_view hex, const float a[3], const float b[3], const float c[3],
                 std::vector<PaintLeaf>& out);

}  // namespace v3d
