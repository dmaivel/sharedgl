#ifdef _WIN32 /* globbed */

#include <windows.h>
#include <client/platform/windrv.h>
#include <client/glimpl.h>

VOID Main()
{
#ifdef OVERRIDE_OPENGL32
    WglInit();
#endif
    glimpl_init();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) 
{
    if (reason == DLL_PROCESS_ATTACH) {
        WinDrvSetModuleAddress(module);
        Main();
    }
    if (reason == DLL_PROCESS_DETACH)
        glimpl_goodbye();

    return TRUE;
}

#endif