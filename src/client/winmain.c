#ifdef _WIN32 /* globbed */

#include <windows.h>
#include <client/platform/windrv.h>
#include <client/glimpl.h>
#include <string.h>

static char env_value[16];

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
        /*
         * some weird hack that can speed up processes, reducing apparent lag;
         * by setting the priority of the program to "low", we give the kernel
         * more CPU time, which is crucial because the kernel driver is how data
         * between the VM and host move.
         *
         * appears to benefit systems with a single core the most
         */
        DWORD result = GetEnvironmentVariableA("SGL_RUN_WITH_LOW_PRIORITY", env_value, 16);
        if (result == 0 || strcmp(env_value, "true") == 0)
            SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

        /*
         * if running a directx application via WineD3D, we don't need to perform a 
         * vertical flip as the library appears to perform this action for us
         */
        result = GetEnvironmentVariableA("SGL_WINED3D_DONT_VFLIP", env_value, 16);
        if (strcmp(env_value, "true") == 0)
            WinDrvSetVflip(FALSE);
        
        WinDrvSetModuleAddress(module);
        Main();
    }
    if (reason == DLL_PROCESS_DETACH)
        glimpl_goodbye();

    return TRUE;
}

#endif