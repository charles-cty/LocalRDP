#include "common.hpp"

int EnsureChildSessionsForStart() {
    if (ChildSessionsEnabled()) return 0;

    if (!WTSEnableChildSessions(TRUE)) {
        DWORD err = GetLastError();
        if (!IsElevated()) {
            PrintOut(L"Enabling Windows child sessions needs a one-time administrator approval.");
            return RelaunchElevated(CommandLineTail());
        }
        PrintErr(L"WTSEnableChildSessions failed: " + FormatWin32(err));
        return static_cast<int>(ExitCode::Error);
    }
    if (!ChildSessionsEnabled()) {
        PrintErr(L"Child sessions are still disabled after WTSEnableChildSessions.");
        return static_cast<int>(ExitCode::Error);
    }
    LogInfo(L"Child sessions enabled");
    PrintOut(L"Windows child sessions enabled.");
    return 0;
}
