// Logs every C++ exception as it is thrown, with the exception type and the guest address that was
// executing. Linked with -Wl,--wrap=__cxa_throw so the real libstdc++ implementation still runs.
//
// The port reports "std::terminate() without current exception" from inside the game's main(), which
// on its own says nothing about what threw or where. This names both.
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <typeinfo>
#include <unwind.h>
#include <cerrno>
#include <pthread.h>

#include "recomp_mod_loader.h"

extern "C" void switch_trace_mark(const char*);
extern "C" void __real___cxa_throw(void* thrown, void* type, void (*destructor)(void*));

namespace {
// Collects host return addresses so a throwing call can be named instead of inferred from the guest
// dispatch address (which is only the last Invoke*Cpu, not the throw site).
struct TraceState {
    char text[400];
    int length = 0;
    int frames = 0;
};

_Unwind_Reason_Code trace_frame(_Unwind_Context* context, void* argument) {
    auto* state = static_cast<TraceState*>(argument);
    const uintptr_t address = static_cast<uintptr_t>(_Unwind_GetIP(context));
    if (address == 0 || state->length > 300) {
        return _URC_NO_REASON;
    }
    const int written = std::snprintf(state->text + state->length,
                                      sizeof(state->text) - static_cast<size_t>(state->length),
                                      " %llx", static_cast<unsigned long long>(address));
    if (written > 0) {
        state->length += written;
    }
    ++state->frames;
    return state->frames >= 14 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
} // namespace

extern "C" void __wrap___cxa_throw(void* thrown, void* type, void (*destructor)(void*)) {
    const auto* info = static_cast<const std::type_info*>(type);
    const char* typeName = info != nullptr ? info->name() : "(null)";
    char line[256];
    // Every libstdc++ exception derives from std::exception, so the thrown object's vtable is valid
    // for what(). Only do this for std:: types to avoid reading through an unrelated object.
    if (typeName != nullptr && typeName[0] == 'S' && typeName[1] == 't') {
        const auto* standard = static_cast<const std::exception*>(thrown);
        std::snprintf(line, sizeof line, "THROW %s from guest=%08x what=%s", typeName,
                      static_cast<unsigned>(RecompMod::CurrentTranslatedExecutionAddress()),
                      standard->what());
    } else {
        std::snprintf(line, sizeof line, "THROW %s from guest=%08x", typeName,
                      static_cast<unsigned>(RecompMod::CurrentTranslatedExecutionAddress()));
    }
    switch_trace_mark(line);
    {
        TraceState state;
        _Unwind_Backtrace(&trace_frame, &state);
        char trace[420];
        std::snprintf(trace, sizeof trace, "TRACE%s", state.text);
        switch_trace_mark(trace);
    }
    __real___cxa_throw(thrown, type, destructor);
}

// std::terminate is entered with no active exception here, so the throw log above does not cover it.
// Wrapping it records the guest address that was executing when something asked to terminate.
extern "C" void __real__ZSt9terminatev();

extern "C" void __wrap__ZSt9terminatev() {
    char line[128];
    std::snprintf(line, sizeof line, "TERMINATE from guest=%08x",
                  static_cast<unsigned>(RecompMod::CurrentTranslatedExecutionAddress()));
    switch_trace_mark(line);
    __real__ZSt9terminatev();
}

// std::call_once turns __gthread_once into std::system_error(errno). Record failures and
// their errno without flooding the SD card for every successful call.
extern "C" int __real_pthread_once(pthread_once_t* once, void (*init)(void));

extern "C" int __wrap_pthread_once(pthread_once_t* once, void (*init)(void)) {
    errno = 0;
    const int result = __real_pthread_once(once, init);
    if (result != 0) {
        char line[128];
        std::snprintf(line, sizeof line, "PTHREAD_ONCE rc=%d errno=%d", result, errno);
        switch_trace_mark(line);
    }
    return result;
}

// libstdc++ throws std::system_error from here. pthread_once was measured returning rc=0 on every
// call, so the errno value libstdc++ decides to throw with is the one fact still missing.
extern "C" void __real__ZSt20__throw_system_errori(int code) __attribute__((noreturn));

extern "C" void __wrap__ZSt20__throw_system_errori(int code) {
    char line[96];
    std::snprintf(line, sizeof line, "THROW_SYSTEM_ERROR code=%d", code);
    switch_trace_mark(line);
    {
        TraceState state;
        _Unwind_Backtrace(&trace_frame, &state);
        char trace[420];
        std::snprintf(trace, sizeof trace, "SETRACE%s", state.text);
        switch_trace_mark(trace);
    }
    __real__ZSt20__throw_system_errori(code);
}
