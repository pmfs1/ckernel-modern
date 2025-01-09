//
// Win32 WINMM emulation
//
#include <os.h>
#include <sys/types.h>
#include <win32.h>

MMRESULT WINAPI
timeBeginPeriod(
        UINT
uPeriod) {
TRACE("timeBeginPeriod");
syslog(LOG_DEBUG,
"timeBeginPeriod: timer resolution is %d", uPeriod);
return 0;
}

MMRESULT WINAPI
timeEndPeriod(
        UINT
uPeriod) {
TRACE("timeEndPeriod");
syslog(LOG_DEBUG,
"timeEndPeriod: timer resolution is %d", uPeriod);
return 0;
}

DWORD WINAPI
timeGetTime(VOID) {
        TRACE("timeGetTime");
        return clock();
}

int __stdcall DllMain(handle_t hmod, int reason, void *reserved) {
    return TRUE;
}
