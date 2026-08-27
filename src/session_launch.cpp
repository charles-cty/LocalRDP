#include "common.hpp"

#include <userenv.h>

#include <vector>

namespace {

DWORD ScoreProcess(const WCHAR* name) {
    if (!name) return 0;
    if (_wcsicmp(name, L"explorer.exe") == 0) return 100;
    if (_wcsicmp(name, L"userinit.exe") == 0) return 80;
    if (_wcsicmp(name, L"rdpshell.exe") == 0) return 70;
    if (_wcsicmp(name, L"sihost.exe") == 0) return 60;
    if (_wcsicmp(name, L"RuntimeBroker.exe") == 0) return 40;
    if (_wcsicmp(name, L"winlogon.exe") == 0) return 0;
    if (_wcsicmp(name, L"csrss.exe") == 0) return 0;
    if (_wcsicmp(name, L"smss.exe") == 0) return 0;
    if (_wcsicmp(name, L"services.exe") == 0) return 0;
    if (_wcsicmp(name, L"lsass.exe") == 0) return 0;
    return 10;
}

bool SameUser(PSID sid) {
    if (!sid) return false;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<unsigned char> buf(need);
    BOOL ok = GetTokenInformation(tok, TokenUser, buf.data(), need, &need);
    CloseHandle(tok);
    if (!ok) return false;
    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    return EqualSid(tu->User.Sid, sid) == TRUE;
}

bool PidInSession(DWORD pid, DWORD session_id) {
    DWORD sid = kNoSession;
    if (!ProcessIdToSessionId(pid, &sid)) return false;
    return sid == session_id;
}

void KillPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 1);
    CloseHandle(h);
}

HANDLE OpenSessionProcess(DWORD session_id) {
    EnablePrivilege(SE_DEBUG_NAME);

    WTS_PROCESS_INFOW* info = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count) || !info) {
        LogWarn(L"WTSEnumerateProcesses failed: " + FormatWin32(GetLastError()));
        return nullptr;
    }

    DWORD best_pid = 0;
    DWORD best_score = 0;
    for (DWORD i = 0; i < count; ++i) {
        if (info[i].SessionId != session_id) continue;
        if (info[i].ProcessId == GetCurrentProcessId()) continue;
        DWORD score = ScoreProcess(info[i].pProcessName);
        if (score == 0) continue;
        if (info[i].pUserSid && SameUser(info[i].pUserSid)) score += 20;
        if (score > best_score) {
            best_score = score;
            best_pid = info[i].ProcessId;
        }
    }
    WTSFreeMemory(info);

    if (best_pid == 0) {
        LogWarn(L"No suitable process in child session " + std::to_wstring(session_id));
        return nullptr;
    }

    const DWORD tries[] = {
        PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE | PROCESS_CREATE_PROCESS,
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | PROCESS_CREATE_PROCESS,
        PROCESS_ALL_ACCESS,
    };
    HANDLE h = nullptr;
    for (DWORD access : tries) {
        h = OpenProcess(access, FALSE, best_pid);
        if (h) break;
    }
    if (!h) {
        LogWarn(L"OpenProcess pid=" + std::to_wstring(best_pid) + L" failed: " +
                FormatWin32(GetLastError()));
        return nullptr;
    }
    LogInfo(L"Using session process pid=" + std::to_wstring(best_pid) + L" score=" +
            std::to_wstring(best_score));
    return h;
}

bool AcceptLaunch(DWORD session_id, DWORD pid) {
    if (pid == 0) return false;
    if (PidInSession(pid, session_id)) {
        LogInfo(L"Launched pid=" + std::to_wstring(pid) + L" in session " +
                std::to_wstring(session_id));
        return true;
    }
    DWORD got = kNoSession;
    ProcessIdToSessionId(pid, &got);
    LogWarn(L"New process pid=" + std::to_wstring(pid) + L" is in session " +
            std::to_wstring(got) + L", expected " + std::to_wstring(session_id) +
            L"; terminating it");
    KillPid(pid);
    return false;
}

bool LaunchWithPrimaryToken(HANDLE primary, std::wstring& cmd, const wchar_t* workdir, DWORD* pid) {
    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, primary, FALSE);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessAsUserW(primary, nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, env,
                                   workdir, &si, &pi);
    DWORD err = GetLastError();
    if (env) DestroyEnvironmentBlock(env);
    if (!ok) {
        LogWarn(L"CreateProcessAsUser failed: " + FormatWin32(err));
        return false;
    }
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool LaunchWithWtsToken(DWORD session_id, std::wstring& cmd, const wchar_t* workdir, DWORD* pid) {
    EnablePrivilege(SE_TCB_NAME);
    EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnablePrivilege(SE_INCREASE_QUOTA_NAME);
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session_id, &token)) {
        LogInfo(L"WTSQueryUserToken unavailable: " + FormatWin32(GetLastError()));
        return false;
    }
    bool ok = LaunchWithPrimaryToken(token, cmd, workdir, pid);
    CloseHandle(token);
    return ok;
}

bool LaunchWithProcessToken(HANDLE process, std::wstring& cmd, const wchar_t* workdir, DWORD* pid) {
    HANDLE proc_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &proc_token)) {
        LogWarn(L"OpenProcessToken failed: " + FormatWin32(GetLastError()));
        return false;
    }
    HANDLE primary = nullptr;
    BOOL dup = DuplicateTokenEx(proc_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
                                TokenPrimary, &primary);
    CloseHandle(proc_token);
    if (!dup || !primary) {
        LogWarn(L"DuplicateTokenEx failed: " + FormatWin32(GetLastError()));
        return false;
    }
    bool ok = LaunchWithPrimaryToken(primary, cmd, workdir, pid);
    CloseHandle(primary);
    return ok;
}

bool LaunchAsChild(HANDLE parent, std::wstring& cmd, const wchar_t* workdir, DWORD* pid) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (bytes == 0) return false;
    std::vector<unsigned char> buf(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return false;

    BOOL upd = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent,
                                         sizeof(parent), nullptr, nullptr);
    if (!upd) {
        DeleteProcThreadAttributeList(list);
        LogWarn(L"UpdateProcThreadAttribute failed: " + FormatWin32(GetLastError()));
        return false;
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    // Inherit the parent's window station/desktop; do not force winsta0\default of *this* session.
    siex.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    siex.StartupInfo.wShowWindow = SW_SHOWNORMAL;
    siex.lpAttributeList = list;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                                 CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, workdir, &siex.StartupInfo, &pi);
    DWORD err = GetLastError();
    DeleteProcThreadAttributeList(list);
    if (!ok) {
        LogWarn(L"CreateProcess (parent attribute) failed: " + FormatWin32(err));
        return false;
    }
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

bool LaunchInSession(DWORD session_id, const std::wstring& command, const std::wstring& workdir,
                     DWORD* pid_out) {
    if (command.empty() || session_id == kNoSession) return false;
    const wchar_t* wd = workdir.empty() ? nullptr : workdir.c_str();

    for (int attempt = 0; attempt < 8; ++attempt) {
        if (attempt > 0) Sleep(400);
        std::wstring cmd = command;
        DWORD pid = 0;

        if (LaunchWithWtsToken(session_id, cmd, wd, &pid) && AcceptLaunch(session_id, pid)) {
            if (pid_out) *pid_out = pid;
            return true;
        }

        HANDLE proc = OpenSessionProcess(session_id);
        if (!proc) continue;

        cmd = command;
        pid = 0;
        if (LaunchWithProcessToken(proc, cmd, wd, &pid) && AcceptLaunch(session_id, pid)) {
            CloseHandle(proc);
            if (pid_out) *pid_out = pid;
            return true;
        }

        cmd = command;
        pid = 0;
        if (LaunchAsChild(proc, cmd, wd, &pid) && AcceptLaunch(session_id, pid)) {
            CloseHandle(proc);
            if (pid_out) *pid_out = pid;
            return true;
        }
        CloseHandle(proc);
    }
    return false;
}
