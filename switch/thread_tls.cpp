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

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "switch_tls.h"

extern "C" void switch_trace_mark(const char*);
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
    errno = 0;
    const int result = __real_pthread_create(thread, attr, &tls_start_routine, request);
    if (result != 0) {
        char line[96];
        std::snprintf(line, sizeof line, "PTHREAD_CREATE failed rc=%d errno=%d", result, errno);
        switch_trace_mark(line);
        std::free(request);
    }
    return result;
}


namespace {

void LogPthreadFailure(const char* name, int result) {
    char line[96];
    std::snprintf(line, sizeof line, "PTHREAD %s rc=%d", name, result);
    switch_trace_mark(line);
}

} // namespace

// libstdc++ turns a non-zero return from any of these into std::system_error. The retrace scheduler
// throws ENOSYS from inside std::call_once, and these are the remaining sources.
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                             const struct timespec* abstime);

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    const int result = __real_pthread_mutex_lock(mutex);
    if (result != 0) {
        LogPthreadFailure("pthread_mutex_lock", result);
    }
    return result;
}

extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    const int result = __real_pthread_mutex_trylock(mutex);
    if (result != 0 && result != 16 /* EBUSY is expected */) {
        LogPthreadFailure("pthread_mutex_trylock", result);
    }
    return result;
}

extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    const int result = __real_pthread_cond_wait(cond, mutex);
    if (result != 0) {
        LogPthreadFailure("pthread_cond_wait", result);
    }
    return result;
}

extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                             const struct timespec* abstime) {
    const int result = __real_pthread_cond_timedwait(cond, mutex, abstime);
    if (result != 0 && result != 110 /* ETIMEDOUT is expected */) {
        LogPthreadFailure("pthread_cond_timedwait", result);
    }
    return result;
}
