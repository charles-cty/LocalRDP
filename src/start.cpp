#include "common.hpp"

int CmdStart(const StartOptions& opt) {
    if (InsideChildSession()) {
        PrintErr(L"Refusing to start: this process is already inside the child session.");
        return static_cast<int>(ExitCode::Error);
    }

    int prep = EnsureChildSessionsForStart();
    if (prep != 0) return prep;

    EnvReport r = QueryEnvironment();
    if (r.blocked()) {
        PrintStatus(r);
        PrintErr(L"");
        PrintErr(L"Cannot start a child session from this environment.");
        return static_cast<int>(ExitCode::NotReady);
    }

    if (GetSystemMetrics(SM_REMOTESESSION)) {
        PrintOut(L"Note: this process is already in a Remote Desktop session. Child sessions are more reliable from the console session.");
        LogWarn(L"SM_REMOTESESSION is set");
    }

    if (HasActiveChildSession()) {
        DWORD id = GetChildSessionId();
        PrintErr(L"A child session already exists (id=" + std::to_wstring(id) +
                 L"). Run `localrdp stop` first. Windows allows only one child session.");
        return static_cast<int>(ExitCode::Conflict);
    }

    std::wstring job;
    if (!opt.command.empty()) {
        job = WriteJobFile(opt);
        if (job.empty()) {
            PrintErr(L"Failed to write the session job file.");
            return static_cast<int>(ExitCode::Error);
        }
        LogInfo(L"Job file " + job);
    }

    int rc = RunRdpSession(opt, job);
    DeleteJobFile(job);
    return rc;
}

int CmdStop() {
    DWORD id = GetChildSessionId();
    if (id == kNoSession || id == 0) {
        PrintOut(L"No child session is active.");
        return static_cast<int>(ExitCode::Ok);
    }
    if (!WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, id, TRUE)) {
        DWORD err = GetLastError();
        PrintErr(L"WTSLogoffSession failed: " + FormatWin32(err));
        return static_cast<int>(ExitCode::Error);
    }
    PrintOut(L"Logged off child session " + std::to_wstring(id) + L".");
    return static_cast<int>(ExitCode::Ok);
}
