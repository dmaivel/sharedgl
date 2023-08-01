#ifdef _WIN32 /* globbed */

#include <windows.h>
#include <client/platform/wgl.h>
#include <client/glimpl.h>

VOID Main()
{
    WglInit();
    glimpl_init();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) 
{
    if (reason == DLL_PROCESS_ATTACH)
        Main();
    if (reason == DLL_PROCESS_DETACH)
        glimpl_goodbye();

    return TRUE;
}

#endif