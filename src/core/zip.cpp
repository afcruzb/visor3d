#include "zip.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <libdeflate.h>

#include "mapped_file.h"

namespace v3d {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

constexpr uint32_t kEocd = 0x06054b50, kEocd64 = 0x06064b50, kEocd64Loc = 0x07064b50;
constexpr uint32_t kCentral = 0x02014b50, kLocal = 0x04034b50;

bool sameName(std::string_view a, std::string_view b) {
    if (!a.empty() && a[0] == '/') a.remove_prefix(1);
    if (!b.empty() && b[0] == '/') b.remove_prefix(1);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

bool ZipArchive::open(const uint8_t* d, size_t n, std::string& error) {
    data_ = d, size_ = n;
    entries_.clear();
    if (n < 22) return error = "ZIP demasiado pequeño", false;

    // End of central directory: last 22 bytes plus an optional comment (<64 KiB).
    size_t eocd = SIZE_MAX;
    size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = n - 22 + 1; i-- > lowest;)
        if (rd32(d + i) == kEocd) {
            eocd = i;
            break;
        }
    if (eocd == SIZE_MAX) return error = "ZIP sin directorio central", false;

    uint64_t count = rd16(d + eocd + 10);
    uint64_t cdSize = rd32(d + eocd + 12);
    uint64_t cdOffset = rd32(d + eocd + 16);
    if (eocd >= 20 && rd32(d + eocd - 20) == kEocd64Loc) {
        uint64_t off = rd64(d + eocd - 20 + 8);
        if (off + 56 <= n && rd32(d + off) == kEocd64) {
            count = rd64(d + off + 32);
            cdSize = rd64(d + off + 40);
            cdOffset = rd64(d + off + 48);
        }
    }
    if (cdOffset > n || cdSize > n - cdOffset) return error = "ZIP con directorio central fuera de rango", false;

    entries_.reserve(size_t(std::min<uint64_t>(count, cdSize / 46)));
    const uint8_t* p = d + cdOffset;
    const uint8_t* end = p + cdSize;
    while (p + 46 <= end && rd32(p) == kCentral) {
        Entry e;
        e.method = rd16(p + 10);
        e.compressedSize = rd32(p + 20);
        e.size = rd32(p + 24);
        uint16_t nameLen = rd16(p + 28), extraLen = rd16(p + 30), commentLen = rd16(p + 32);
        e.localOffset = rd32(p + 42);
        if (p + 46 + nameLen + extraLen > end) break;
        e.name = std::string_view(reinterpret_cast<const char*>(p + 46), nameLen);

        // ZIP64 extra field: only the fields saturated in the header, in order.
        const uint8_t* x = p + 46 + nameLen;
        const uint8_t* xend = x + extraLen;
        while (x + 4 <= xend) {
            uint16_t id = rd16(x), len = rd16(x + 2);
            const uint8_t* f = x + 4;
            if (id == 0x0001) {
                if (e.size == 0xFFFFFFFF && f + 8 <= x + 4 + len) e.size = rd64(f), f += 8;
                if (e.compressedSize == 0xFFFFFFFF && f + 8 <= x + 4 + len) e.compressedSize = rd64(f), f += 8;
                if (e.localOffset == 0xFFFFFFFF && f + 8 <= x + 4 + len) e.localOffset = rd64(f);
            }
            x += 4 + len;
        }
        entries_.push_back(e);
        p += 46 + nameLen + extraLen + commentLen;
    }
    if (entries_.empty()) return error = "ZIP vacío", false;
    return true;
}

const ZipArchive::Entry* ZipArchive::find(std::string_view name) const {
    for (const Entry& e : entries_)
        if (sameName(e.name, name)) return &e;
    return nullptr;
}

bool ZipArchive::extract(const Entry& e, Buf<char>& out, std::string& error) const {
    if (e.localOffset + 30 > size_ || rd32(data_ + e.localOffset) != kLocal) return error = "entrada ZIP dañada", false;
    // The local header has its own name/extra lengths, which may differ from the central ones.
    const uint8_t* h = data_ + e.localOffset;
    uint64_t start = e.localOffset + 30 + rd16(h + 26) + rd16(h + 28);
    if (start > size_ || e.compressedSize > size_ - start) return error = "entrada ZIP truncada", false;
    const uint8_t* src = data_ + start;
    prefetch(src, size_t(e.compressedSize));  // only this entry's bytes are read

    out.resize(size_t(e.size));
    if (e.method == 0) {
        if (e.compressedSize != e.size) return error = "entrada ZIP dañada", false;
        std::memcpy(out.data(), src, size_t(e.size));
        return true;
    }
    if (e.method != 8) return error = "método de compresión ZIP no soportado", false;

    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) return error = "sin memoria", false;
    size_t actual = 0;
    libdeflate_result r =
        libdeflate_deflate_decompress(dec, src, size_t(e.compressedSize), out.data(), out.size(), &actual);
    libdeflate_free_decompressor(dec);
    if (r != LIBDEFLATE_SUCCESS || actual != e.size) return error = "error al descomprimir " + std::string(e.name), false;
    return true;
}

}  // namespace v3d
