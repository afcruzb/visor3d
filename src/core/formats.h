#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "loader.h"
#include "mesh.h"

namespace v3d {

// Each loader fills `scene` (meshes, instances, palette) or sets scene.error.
// Bounds are computed afterwards by the dispatcher.
void loadStl(const uint8_t* data, size_t size, Scene& scene);
void load3mf(const uint8_t* data, size_t size, Scene& scene);
void loadStep(const std::string& path, const LoadOptions& options, Scene& scene);

}  // namespace v3d
