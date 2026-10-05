#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mesh.h"

namespace v3d {

// Read-only ZIP reader over a memory buffer: central directory (with ZIP64),
// stored and deflated entries. Enough for OPC packages such as 3MF.
class ZipArchive {
public:
    struct Entry {
        std::string_view name;
        uint64_t localOffset = 0, compressedSize = 0, size = 0;
        uint16_t method = 0;
    };

    bool open(const uint8_t* data, size_t size, std::string& error);

    // Lookup is case-insensitive (OPC part names are) and ignores a leading '/'.
    const Entry* find(std::string_view name) const;
    const std::vector<Entry>& entries() const { return entries_; }

    // Decompresses an entry. Thread-safe: each call uses its own decompressor.
    bool extract(const Entry& e, Buf<char>& out, std::string& error) const;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::vector<Entry> entries_;
};

}  // namespace v3d
