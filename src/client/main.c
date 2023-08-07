#include <client/glimpl.h>

#ifndef _WIN32
#include <client/platform/glx.h>
#endif

void __attribute__((constructor)) sharedgl_entry(void) 
{
    glimpl_init();
#ifndef _WIN32
    glximpl_init();
#endif
}

void __attribute__((destructor)) sharedgl_goodbye(void) 
{
    glimpl_goodbye();
}