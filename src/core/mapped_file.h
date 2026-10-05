#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace v3d {

// Read-only memory map of a whole file. Nothing is read up front: pages fault
// in on first touch, so containers (3MF) only read the entries they use.
// Callers that will read a range completely prefetch() it first.
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();

    bool open(const std::string& path, std::string* error);

    const uint8_t* data() const { return static_cast<const uint8_t*>(ptr_); }
    size_t size() const { return size_; }

private:
    void* ptr_ = nullptr;
    size_t size_ = 0;
};

// Pre-faults [p, p + n) of a mapping in one call (MADV_POPULATE_READ): for data
// read completely and in order this beats taking thousands of page faults.
void prefetch(const void* p, size_t n);

}  // namespace v3d
