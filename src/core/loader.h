#pragma once

#include <string>
#include <string_view>

#include "mesh.h"

namespace v3d {

enum class Format { Unknown, Stl, ThreeMF, Step };

Format formatFromExtension(std::string_view path);
inline bool isSupportedFile(std::string_view path) { return formatFromExtension(path) != Format::Unknown; }

struct LoadOptions {
    // Read and fill the STEP tessellation cache (~/.cache/visor3d).
    bool useCache = true;
    // False for callers that must not block for seconds (the Dolphin
    // thumbnailer): big STEP files then load only from the cache.
    bool allowSlowImport = true;
};

// Loads any supported file. On failure scene.error is set and the scene is empty.
// Thread-safe: no global state besides the lazily dlopen()ed STEP plugin.
Scene loadScene(const std::string& path, const LoadOptions& options = {});

}  // namespace v3d
