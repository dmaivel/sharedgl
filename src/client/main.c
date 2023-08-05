#include <client/glimpl.h>

void __attribute__((constructor)) sharedgl_entry(void) 
{
    glimpl_init();
}

void __attribute__((destructor)) sharedgl_goodbye(void) 
{
    glimpl_goodbye();
}