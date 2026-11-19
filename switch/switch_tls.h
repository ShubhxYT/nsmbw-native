// Self-calibrating main-thread TLS bootstrap.
//
// An NSO carries no PT_TLS, and nothing in libnx ever writes tpidr_el0 (every libnx routine reads
// tpidrro_el0, which is the kernel-provided IPC buffer). The register is therefore left at 0, so the
// first access to any thread_local dereferences (0 + tls_offset): toml11 caches its parser spec in a
// thread_local and dies at 0xCA0, which Atmosphère reports as "Fault Address: 0".
//
// The linker computes TLS offsets against a base that is not __tdata_lma, so pointing tpidr_el0 at
// .tdata blindly is not enough. Instead this walks the base back until a sentinel thread_local reads
// back its initial value, which uses the compiler's own addressing and therefore stays correct
// whatever the linker decided.
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

// Written by Bootstrap so a run can report how it went without needing stdio.
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
// tpidr_el0 still 0 the hoisted address is 0x10 and the read faults before calibration begins.
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
    if (__tdata_lma == nullptr || __tls_start == nullptr) {
        g_switchTlsResult = static_cast<int>(TlsResult::NoSymbols);
        return g_switchTlsResult;
    }

    // .tdata is writable and .tbss follows it in the same segment, so the block lives where the
    // initial data already is; only the base pointer has to be found.
    const uint64_t tdata = reinterpret_cast<uint64_t>(__tdata_lma);
    for (int step = 0; step <= 64; ++step) {
        const uint64_t delta = static_cast<uint64_t>(step) * 8ull;
        Set(tdata - delta);
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
