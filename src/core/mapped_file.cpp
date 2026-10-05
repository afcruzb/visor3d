#include "mapped_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace v3d {

MappedFile::~MappedFile() {
    if (ptr_) munmap(ptr_, size_);
}

bool MappedFile::open(const std::string& path, std::string* error) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        if (error) *error = std::strerror(errno);
        ::close(fd);
        return false;
    }
    if (st.st_size <= 0) {
        if (error) *error = "archivo vacío";
        ::close(fd);
        return false;
    }
    size_ = size_t(st.st_size);
    void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        if (error) *error = std::strerror(errno);
        size_ = 0;
        return false;
    }
    ptr_ = p;
    return true;
}

void prefetch(const void* p, size_t n) {
    if (n == 0) return;
    static const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    uintptr_t begin = uintptr_t(p) & ~(page - 1);
    uintptr_t end = uintptr_t(p) + n;
    // Pre-5.14 kernels lack MADV_POPULATE_READ: fall back to async readahead.
    if (madvise(reinterpret_cast<void*>(begin), end - begin, MADV_POPULATE_READ) != 0)
        madvise(reinterpret_cast<void*>(begin), end - begin, MADV_WILLNEED);
}

}  // namespace v3d
