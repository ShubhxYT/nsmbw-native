#pragma once
// Horizon has no dynamic loader: an .nro is a single statically linked image. Dawn includes
// <dlfcn.h> for its plugin path only, so these report "symbol not found" and the callers that
// matter (the EGL/GLES loader) go through eglGetProcAddress instead.

#define RTLD_LAZY 0
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0
#define RTLD_NOLOAD 0
#define RTLD_NODELETE 0
#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif

static inline void* dlopen(const char* filename, int flags) {
    (void)filename;
    (void)flags;
    return 0;
}

static inline void* dlsym(void* handle, const char* symbol) {
    (void)handle;
    (void)symbol;
    return 0;
}

static inline int dlclose(void* handle) {
    (void)handle;
    return 0;
}

static inline char* dlerror(void) { return (char*)"dlopen is not available on Horizon"; }

#ifdef __cplusplus
}
#endif
