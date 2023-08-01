#ifndef _SGL_HOOK_H_
#define _SGL_HOOK_H_

void hook_install(const char *name, void *fn);

char *getlib_fullname(const char *name);

void *real_dlopen(const char *name, int flags);
void *real_dlsym(void *h, const char* name);

#endif