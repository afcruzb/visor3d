#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "loader.h"

namespace v3d {

// PNG preview embedded in a 3MF (Bambu Studio / OrcaSlicer plate image or the
// package thumbnail), passed through as stored.
struct EncodedPng {
    std::vector<uint8_t> bytes;
};

// Software render: RGBA8, rows top to bottom, transparent background.
struct RgbaImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

using Thumbnail = std::variant<EncodedPng, RgbaImage>;

// The thumbnail policy shared by the CLI and the Dolphin plugin: the embedded
// 3MF preview when there is one, otherwise an isometric software render with
// the viewer's lighting. Returns nullopt (and the reason) if the file cannot
// be loaded.
std::optional<Thumbnail> makeThumbnail(const std::string& path, int width, int height, const LoadOptions& options,
                                       std::string* error = nullptr);

}  // namespace v3d
