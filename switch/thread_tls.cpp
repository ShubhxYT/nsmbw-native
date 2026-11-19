// Gives every thread its own TLS block.
//
// libnx never writes tpidr_el0, so only the main thread (bootstrapped from __appInit) has valid
// thread-local storage; every thread created later starts with tpidr_el0 == 0 and any thread_local in
// it resolves against address 0. libstdc++'s std::call_once keeps `__once_callable`/`__once_call` in
// thread_locals, so a call_once reached from a worker thread misbehaves - which is how the retrace
// scheduler ended up throwing std::system_error(ENOSYS) into guest code.
//
// pthread_create is wrapped (-Wl,--wrap=pthread_create) so the thread's entry point attaches TLS
// before the real routine runs.
#include <pthread.h>

#include <cstdlib>

#include "switch_tls.h"

extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*entry)(void*), void* argument);

namespace {

struct StartRequest {
    void* (*entry)(void*);
    void* argument;
};

void* tls_start_routine(void* raw) {
    auto* request = static_cast<StartRequest*>(raw);
    const auto entry = request->entry;
    void* const argument = request->argument;
    std::free(request);

    SwitchTlsAttach();
    return entry(argument);
}

} // namespace

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*entry)(void*), void* argument) {
    if (entry == nullptr) {
        return __real_pthread_create(thread, attr, entry, argument);
    }
    auto* request = static_cast<StartRequest*>(std::malloc(sizeof(StartRequest)));
    if (request == nullptr) {
        return __real_pthread_create(thread, attr, entry, argument);
    }
    request->entry = entry;
    request->argument = argument;
    return __real_pthread_create(thread, attr, &tls_start_routine, request);
}
