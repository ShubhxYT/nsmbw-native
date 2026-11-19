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

#include "recomp_mod_loader.h"

extern "C" void switch_trace_mark(const char*);
extern "C" void __real___cxa_throw(void* thrown, void* type, void (*destructor)(void*));

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
