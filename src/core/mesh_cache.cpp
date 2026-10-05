#include "mesh_cache.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace v3d {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kMagic = 0x43443356;  // "V3DC"
// Bump when the tessellation parameters or the file layout change.
constexpr uint32_t kVersion = 1;
constexpr uint64_t kMaxCacheBytes = 512ull << 20;

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string cacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/visor3d";
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.cache/visor3d" : std::string();
}

struct Header {
    uint32_t magic, version;
    uint64_t meshes, instances, palette;
};

template <class V>
bool writeVec(FILE* f, const V& v) {
    uint64_t n = v.size();
    return std::fwrite(&n, 8, 1, f) == 1 && (n == 0 || std::fwrite(v.data(), sizeof(v[0]), n, f) == n);
}

template <class V>
bool readVec(FILE* f, V& v, uint64_t limit) {
    uint64_t n;
    if (std::fread(&n, 8, 1, f) != 1 || n > limit) return false;
    v.resize(size_t(n));
    return n == 0 || std::fread(v.data(), sizeof(v[0]), n, f) == n;
}

// Keeps the cache directory under kMaxCacheBytes by dropping the oldest files.
void prune(const std::string& dir) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        total += e.file_size(ec);
        files.emplace_back(e.last_write_time(ec), e.path());
    }
    if (total <= kMaxCacheBytes) return;
    std::sort(files.begin(), files.end());
    for (const auto& [t, p] : files) {
        if (total <= kMaxCacheBytes * 3 / 4) break;
        uint64_t sz = fs::file_size(p, ec);
        if (fs::remove(p, ec)) total -= sz;
    }
}

}  // namespace

std::string cachePathFor(const std::string& source) {
    std::string dir = cacheDir();
    if (dir.empty()) return {};
    char real[PATH_MAX];
    if (!realpath(source.c_str(), real)) return {};
    struct stat st;
    if (stat(real, &st) != 0) return {};
    uint64_t h = fnv1a(real, std::strlen(real));
    h = fnv1a(&st.st_size, sizeof st.st_size, h);
    h = fnv1a(&st.st_mtim, sizeof st.st_mtim, h);
    h = fnv1a(&kVersion, sizeof kVersion, h);
    char name[32];
    std::snprintf(name, sizeof name, "/%016llx.mesh", static_cast<unsigned long long>(h));
    return dir + name;
}

bool cacheLoad(const std::string& path, Scene& scene) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    Header h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == kMagic && h.version == kVersion &&
              h.meshes < (1u << 20) && h.instances < (1u << 24) && h.palette >= 1 && h.palette <= 0x10000;
    constexpr uint64_t kLimit = 1ull << 32;
    Scene s;
    if (ok) {
        s.meshes.resize(size_t(h.meshes));
        for (Mesh& m : s.meshes) {
            ok = ok && readVec(f, m.positions, kLimit) && readVec(f, m.normals, kLimit) &&
                 readVec(f, m.indices, kLimit) && readVec(f, m.triColor, kLimit);
            if (!ok) break;
        }
    }
    if (ok) {
        s.instances.resize(size_t(h.instances));
        for (Instance& in : s.instances) {
            ok = ok && std::fread(&in.mesh, 4, 1, f) == 1 && std::fread(&in.color, 2, 1, f) == 1 &&
                 std::fread(in.transform.m, sizeof in.transform.m, 1, f) == 1;
            if (!ok || in.mesh >= h.meshes) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        s.palette.resize(size_t(h.palette));
        ok = std::fread(s.palette.data(), 4, size_t(h.palette), f) == h.palette;
    }
    std::fclose(f);
    if (!ok) return false;
    // Recently used files survive pruning longer.
    utimensat(AT_FDCWD, path.c_str(), nullptr, 0);
    scene = std::move(s);
    return true;
}

void cacheStore(const std::string& path, const Scene& scene) {
    std::string dir = path.substr(0, path.rfind('/'));
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string tmp = path + ".tmp" + std::to_string(getpid());
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    Header h{kMagic, kVersion, scene.meshes.size(), scene.instances.size(), scene.palette.size()};
    bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
    for (const Mesh& m : scene.meshes)
        ok = ok && writeVec(f, m.positions) && writeVec(f, m.normals) && writeVec(f, m.indices) && writeVec(f, m.triColor);
    for (const Instance& in : scene.instances)
        ok = ok && std::fwrite(&in.mesh, 4, 1, f) == 1 && std::fwrite(&in.color, 2, 1, f) == 1 &&
             std::fwrite(in.transform.m, sizeof in.transform.m, 1, f) == 1;
    ok = ok && std::fwrite(scene.palette.data(), 4, scene.palette.size(), f) == scene.palette.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return;
    }
    prune(dir);
}

}  // namespace v3d
