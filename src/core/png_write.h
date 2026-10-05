#pragma once

#include <cstdint>
#include <string>

namespace v3d {

// Writes 8-bit RGBA pixels (rows top to bottom) as PNG. Returns false on I/O error.
bool writePng(const std::string& path, const uint8_t* rgba, int width, int height);

}  // namespace v3d
