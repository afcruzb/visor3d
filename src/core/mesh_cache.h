#pragma once

#include <string>

#include "mesh.h"

namespace v3d {

// Disk cache of tessellated scenes, for formats that are slow to import (STEP).
// Keyed by absolute path + size + mtime, stored under $XDG_CACHE_HOME/visor3d.

// Cache file path for `source`, or empty if the file cannot be stat()ed.
std::string cachePathFor(const std::string& source);

bool cacheLoad(const std::string& cachePath, Scene& scene);
void cacheStore(const std::string& cachePath, const Scene& scene);

}  // namespace v3d
