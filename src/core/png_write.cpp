#include "png_write.h"

#include <cstdio>
#include <cstring>
#include <libdeflate.h>
#include <vector>

namespace v3d {

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t b[4] = {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)};
    v.insert(v.end(), b, b + 4);
}

void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    put32(out, uint32_t(len));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    put32(out, libdeflate_crc32(0, out.data() + start, len + 4));
}

}  // namespace

bool writePng(const std::string& path, const uint8_t* rgba, int w, int h) {
    // Filter type 0 (none) per row: fine for previews and thumbnails.
    size_t stride = size_t(w) * 4;
    std::vector<uint8_t> raw((stride + 1) * size_t(h));
    for (int y = 0; y < h; ++y) {
        raw[size_t(y) * (stride + 1)] = 0;
        std::memcpy(&raw[size_t(y) * (stride + 1) + 1], rgba + size_t(y) * stride, stride);
    }
    libdeflate_compressor* c = libdeflate_alloc_compressor(6);
    if (!c) return false;
    std::vector<uint8_t> z(libdeflate_zlib_compress_bound(c, raw.size()));
    size_t zlen = libdeflate_zlib_compress(c, raw.data(), raw.size(), z.data(), z.size());
    libdeflate_free_compressor(c);
    if (!zlen) return false;

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t ihdr[13];
    for (int i = 0; i < 4; ++i) ihdr[i] = uint8_t(uint32_t(w) >> (24 - 8 * i));
    for (int i = 0; i < 4; ++i) ihdr[4 + i] = uint8_t(uint32_t(h) >> (24 - 8 * i));
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;  // 8-bit RGBA
    chunk(out, "IHDR", ihdr, sizeof ihdr);
    chunk(out, "IDAT", z.data(), zlen);
    chunk(out, "IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace v3d
