#include <client/hook.h>
#include <client/glimpl.h>
#include <client/platform/glx.h>

void __attribute__((constructor)) sharedgl_entry(void) 
{
    /*
     * platform dependent hooks
     */
    hook_install("glXSwapBuffers", glXSwapBuffers);
    // hook_install("glXGetProcAddressARB", glXGetProcAddressARB);
    // hook_install("glXGetProcAddress", glXGetProcAddress);

    glimpl_init();
}

void __attribute__((destructor)) sharedgl_goodbye(void) 
{
    glimpl_goodbye();
}