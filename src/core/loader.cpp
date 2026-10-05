#include "loader.h"

#include <cctype>
#include <cstring>

#include "formats.h"
#include "mapped_file.h"

namespace v3d {

static bool extIs(std::string_view path, std::string_view ext) {
    if (path.size() < ext.size()) return false;
    std::string_view tail = path.substr(path.size() - ext.size());
    for (size_t i = 0; i < ext.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != ext[i]) return false;
    return true;
}

Format formatFromExtension(std::string_view path) {
    if (extIs(path, ".stl")) return Format::Stl;
    if (extIs(path, ".3mf")) return Format::ThreeMF;
    if (extIs(path, ".step") || extIs(path, ".stp")) return Format::Step;
    return Format::Unknown;
}

static Format sniff(const uint8_t* d, size_t n) {
    if (n >= 4 && std::memcmp(d, "PK\x03\x04", 4) == 0) return Format::ThreeMF;
    if (n >= 12 && std::memcmp(d, "ISO-10303-21", 12) == 0) return Format::Step;
    return Format::Stl;
}

// File data is untrusted: clamp colour references once here so the renderers
// can index the palette directly.
static void clampColorIndices(Scene& scene) {
    const size_t n = scene.palette.size();
    for (Instance& in : scene.instances)
        if (in.color >= n) in.color = 0;
    for (Mesh& m : scene.meshes)
        for (uint16_t& c : m.triColor)
            if (c >= n) c = 0;
}

Scene loadScene(const std::string& path, const LoadOptions& options) {
    Scene scene;
    Format fmt = formatFromExtension(path);

    if (fmt == Format::Step) {
        loadStep(path, options, scene);
    } else {
        MappedFile file;
        if (!file.open(path, &scene.error)) return scene;
        // Content wins over extension: a mislabelled file still opens.
        Format real = sniff(file.data(), file.size());
        if (real == Format::Step) {
            loadStep(path, options, scene);
        } else if (real == Format::ThreeMF) {
            load3mf(file.data(), file.size(), scene);
        } else {
            prefetch(file.data(), file.size());  // STL parsing reads every byte
            loadStl(file.data(), file.size(), scene);
        }
    }

    if (scene.error.empty() && scene.instances.empty()) scene.error = "el archivo no contiene geometría";
    if (!scene.error.empty()) {
        scene.meshes.clear();
        scene.instances.clear();
        return scene;
    }
    clampColorIndices(scene);
    scene.computeBounds();
    if (!scene.bounds.valid()) scene.error = "geometría vacía o no válida";
    return scene;
}

}  // namespace v3d
