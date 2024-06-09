#ifdef _WIN32 /* globbed */

#include <windows.h>
#include <client/platform/windrv.h>
#include <client/glimpl.h>
#include <string.h>

static char sgl_run_with_low_priority_env_value[16];

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
         */

        DWORD result = GetEnvironmentVariableA("SGL_RUN_WITH_LOW_PRIORITY", sgl_run_with_low_priority_env_value, 16);
        if (result == 0 || strcmp(sgl_run_with_low_priority_env_value, "true") == 0)
            SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

        WinDrvSetModuleAddress(module);
        Main();
    }
    if (reason == DLL_PROCESS_DETACH)
        glimpl_goodbye();

    return TRUE;
}

#endif