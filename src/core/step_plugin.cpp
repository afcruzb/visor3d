// STEP support lives in libvisor3d_step.so (OpenCASCADE). It is dlopen()ed on
// first use so STL/3MF never pay for loading the ~30 OCCT shared libraries.

#include <climits>
#include <dlfcn.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "formats.h"
#include "mesh_cache.h"

#ifndef V3D_PLUGIN_DIR
#define V3D_PLUGIN_DIR "/usr/lib/visor3d"
#endif

namespace v3d {

using StepLoadFn = void (*)(const char* path, Scene* scene);

// Uncached imports of files this big take seconds (see LoadOptions::allowSlowImport).
constexpr off_t kMaxUncachedStepBytes = 32 << 20;

static StepLoadFn resolvePlugin(std::string& error) {
    static std::once_flag once;
    static StepLoadFn fn = nullptr;
    static std::string loadError;
    std::call_once(once, [] {
        std::vector<std::string> candidates;
        // Next to the executable first, so a build tree works without installing.
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            std::string dir(buf, size_t(n));
            dir.resize(dir.rfind('/'));
            candidates.push_back(dir + "/libvisor3d_step.so");
        }
        candidates.push_back(V3D_PLUGIN_DIR "/libvisor3d_step.so");
        for (const std::string& c : candidates) {
            void* h = dlopen(c.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (!h) {
                loadError = dlerror();
                continue;
            }
            fn = reinterpret_cast<StepLoadFn>(dlsym(h, "visor3d_load_step"));
            if (fn) return;
            loadError = "símbolo visor3d_load_step no encontrado";
        }
    });
    if (!fn) error = "soporte STEP no disponible: " + loadError;
    return fn;
}

void loadStep(const std::string& path, const LoadOptions& options, Scene& scene) {
    // A cached tessellation skips OpenCASCADE entirely, plugin load included.
    std::string cache = options.useCache ? cachePathFor(path) : std::string();
    if (!cache.empty() && cacheLoad(cache, scene)) return;

    struct stat st;
    if (!options.allowSlowImport && stat(path.c_str(), &st) == 0 && st.st_size > kMaxUncachedStepBytes) {
        scene.error = "STEP grande sin malla en caché";
        return;
    }
    StepLoadFn fn = resolvePlugin(scene.error);
    if (!fn) return;
    fn(path.c_str(), &scene);
    if (scene.error.empty() && !cache.empty()) cacheStore(cache, scene);
}

}  // namespace v3d
