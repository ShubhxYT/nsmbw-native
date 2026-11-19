// Main-thread TLS bootstrap.
//
// An NSO carries no PT_TLS, and nothing in libnx ever writes tpidr_el0 (every libnx routine reads
// tpidrro_el0, which is the kernel-provided IPC buffer). The register is therefore left at 0, so the
// first access to any thread_local dereferences (0 + tls_offset): toml11 caches its parser spec in a
// thread_local and died at 0xCA0, which Atmosphère reported as "Fault Address: 0".
//
// The block must be the reserved .main.tls region, NOT .tdata. switch.ld places .tbss and .bss at
// the same virtual address (both are NOBITS, and TLS addresses are offsets rather than real
// addresses), so pointing tpidr_el0 at .tdata makes every .tbss variable alias a real .bss global.
// That silently overwrote TranslatedFunctionRegistry's vector storage with 1, which is what stopped
// static initialisation from completing.
//
// The linker computes TLS offsets against a base that is not __tls_start, so the delta is calibrated
// at runtime against a sentinel thread_local instead of being assumed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace SwitchTls {

inline uint64_t Current() {
    uint64_t value = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

inline void Set(uint64_t value) {
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(value) : "memory");
}

enum class TlsResult : int {
    AlreadySet = 0,
    Calibrated = 1,
    NoSymbols = 2,
    NoDeltaFound = 3,
};

} // namespace SwitchTls

extern "C" char __tls_start[] __attribute__((weak));
extern "C" char __tls_end[] __attribute__((weak));
extern "C" char __tdata_lma[] __attribute__((weak));
extern "C" char __tdata_lma_end[] __attribute__((weak));

extern "C" int g_switchTlsResult;
extern "C" uint64_t g_switchTlsDelta;

// Sentinel in .tdata: its address as the compiler computes it is exactly what calibration must get
// right.
extern "C" thread_local uint64_t g_switchTlsAnchor;
extern "C" constexpr uint64_t kSwitchTlsSentinel = 0x4E534D4257544C53ull;  // "NSMBWTLS"

// A separate function, so the compiler cannot hoist the mrs/add out of the calibration loop: with
// tpidr_el0 still 0 the hoisted address is the raw TLS offset and the read faults before
// calibration begins.
__attribute__((noinline)) inline bool SwitchTlsAnchorMatches() {
    __asm__ volatile("" ::: "memory");
    const bool matches = g_switchTlsAnchor == kSwitchTlsSentinel;
    __asm__ volatile("" ::: "memory");
    return matches;
}

extern "C" inline int SwitchTlsBootstrap() {
    using namespace SwitchTls;
    if (Current() != 0) {
        g_switchTlsResult = static_cast<int>(TlsResult::AlreadySet);
        return g_switchTlsResult;
    }
    if (__tls_start == nullptr || __tls_end == nullptr || __tdata_lma == nullptr) {
        g_switchTlsResult = static_cast<int>(TlsResult::NoSymbols);
        return g_switchTlsResult;
    }

    // .main.tls is the block reserved for exactly this: .tdata followed by .tbss, and nothing else
    // aliased to it. Seed it with the .tdata image and zero the .tbss tail.
    const size_t total = static_cast<size_t>(__tls_end - __tls_start);
    size_t image = 0;
    if (__tdata_lma_end != nullptr && __tdata_lma_end > __tdata_lma) {
        image = static_cast<size_t>(__tdata_lma_end - __tdata_lma);
    }
    if (image > total) {
        image = total;
    }
    for (size_t i = 0; i < total; ++i) {
        __tls_start[i] = i < image ? __tdata_lma[i] : 0;
    }

    const uint64_t block = reinterpret_cast<uint64_t>(__tls_start);
    for (int step = 0; step <= 64; ++step) {
        const uint64_t delta = static_cast<uint64_t>(step) * 8ull;
        Set(block - delta);
        if (SwitchTlsAnchorMatches()) {
            g_switchTlsDelta = delta;
            g_switchTlsResult = static_cast<int>(TlsResult::Calibrated);
            return g_switchTlsResult;
        }
    }

    Set(0);
    g_switchTlsResult = static_cast<int>(TlsResult::NoDeltaFound);
    return g_switchTlsResult;
}
