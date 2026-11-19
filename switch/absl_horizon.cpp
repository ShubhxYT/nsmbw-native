// The virtual memory primitives Abseil expects, over Horizon's heap.
//
// Abseil's LowLevelAlloc (used by its mutex and hash-table instrumentation) maps anonymous pages.
// Horizon has no mmap, so this provides one backed by page-aligned allocations. Only the shape
// Abseil uses is supported: anonymous private mappings, unmapped as a whole.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr size_t kPageSize = 4096;

// The size is stored in the page immediately before the mapping so munmap can release it.
struct MappingHeader {
    void* base;
    size_t size;
};

} // namespace

extern "C" {

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    if (length == 0) {
        return MAP_FAILED;
    }
    const size_t rounded = (length + kPageSize - 1) & ~(kPageSize - 1);
    auto* base = static_cast<unsigned char*>(std::aligned_alloc(kPageSize, rounded + 2 * kPageSize));
    if (base == nullptr) {
        return MAP_FAILED;
    }
    auto* header = reinterpret_cast<MappingHeader*>(base);
    header->base = base;
    header->size = rounded + 2 * kPageSize;
    std::memset(base + kPageSize, 0, rounded);
    return base + 2 * kPageSize;
}

int munmap(void* addr, size_t length) {
    (void)length;
    if (addr == nullptr) {
        return -1;
    }
    auto* header = reinterpret_cast<MappingHeader*>(static_cast<unsigned char*>(addr) - 2 * kPageSize);
    std::free(header->base);
    return 0;
}

int mprotect(void* addr, size_t len, int prot) {
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

int madvise(void* addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

long sysconf(int name) {
    (void)name;
    return static_cast<long>(kPageSize);
}

int getpagesize(void) { return static_cast<int>(kPageSize); }

} // extern "C"
