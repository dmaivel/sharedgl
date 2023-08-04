#include <client/hook.h>
#include <elfhacks.h>
#include <stdio.h>
#include <string.h>

struct hook {
    char name[128];
    void *fn;
} hooks[2048];

struct lib {
    char name[128];
} libs[256];

int hookc = 0;
int libc = 0;

void *(*__dlopen)(const char *, int) = NULL;
void *(*__dlsym)(void *, const char *) = NULL;

void hook_install(const char *name, void *fn)
{
    hooks[hookc++] = (struct hook){
        .name = "",
        .fn = fn
    };
    strcpy(hooks[hookc - 1].name, name);
}

void __dlinit()
{
    eh_obj_t libdl;
    int ret;

    const char* libs[] = {
#if defined(__GLIBC__)
        "*libdl.so*",
#endif
        "*libc.so*",
        "*libc.*.so*",
    };

    for (size_t i = 0; i < sizeof(libs) / sizeof(*libs); i++)
    {
        ret = eh_find_obj(&libdl, libs[i]);
        if (ret)
            continue;

        eh_find_sym(&libdl, "dlopen", (void **) &__dlopen);
        eh_find_sym(&libdl, "dlsym", (void **) &__dlsym);
        eh_destroy_obj(&libdl);

        if (__dlopen && __dlsym)
            break;
        __dlopen = NULL;
        __dlsym = NULL;
    }
}

char *getlib_fullname(const char *name)
{
    for (int i = 0; i < libc; i++) {
        if (strstr(libs[i].name, name))
            return libs[i].name;
    }
    return NULL;
}

static void addlib(const char *lib)
{
    if (lib == NULL)
        return;
    if (getlib_fullname(lib) == NULL)
        strcpy(libs[libc++].name, lib);
}

void *dlopen(const char *name, int flags)
{
    if (!__dlopen)
        __dlinit();

    addlib(name);
    return __dlopen(name, flags);
}

void *dlsym(void *h, const char* name)
{
    if (!__dlsym)
        __dlinit();

    for (int i = 0; i < hookc; i++)
        if (strcmp(name, hooks[i].name) == 0)
            return hooks[i].fn;

    /*
     * this is a fix for applications that use libepoxy
     *
     * blacklist glx and egl, only allowing opengl functions
     * to be returned without explicitly hooking them.
     */
    if (strstr(name, "gl") && strstr(name, "glX") == NULL && strstr(name, "egl") == NULL) {
        void *internal_gl_function = __dlsym(NULL, name);
        if (internal_gl_function)
            return internal_gl_function;
    }

    return __dlsym(h, name);
}

void *real_dlopen(const char *name, int flags)
{
    return __dlopen(name, flags);
}

void *real_dlsym(void *h, const char* name)
{
    return __dlsym(h, name);
}