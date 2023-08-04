#include <client/hook.h>
#include <client/glimpl.h>
#include <client/platform/glx.h>

void __attribute__((constructor)) sharedgl_entry(void) 
{
    glimpl_init();
}

void __attribute__((destructor)) sharedgl_goodbye(void) 
{
    glimpl_goodbye();
}