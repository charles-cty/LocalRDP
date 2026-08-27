#include "common.hpp"

#include <thread>
#include <atomic>

namespace {

constexpr wchar_t kDialogClass[] = L"#32770";
constexpr wchar_t kResumeSuffix[] = L"\\CrossDeviceResume.exe";

std::atomic<bool> g_run{true};

bool TitleMatches(const wchar_t* title) {
    if (!title || !title[0]) return false;
    const size_t n = wcslen(title);
    const size_t s = wcslen(kResumeSuffix);
    if (n < s) return false;
    return _wcsicmp(title + (n - s), kResumeSuffix) == 0;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, kDialogClass) != 0) return TRUE;
    int n = GetWindowTextLengthW(hwnd);
    if (n <= 0) return TRUE;
    std::wstring title(n + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), n + 1);
    title.resize(wcslen(title.c_str()));
    if (!TitleMatches(title.c_str())) return TRUE;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ++*reinterpret_cast<int*>(lp);
    LogInfo(L"[session] dismissed system dialog: " + title);
    return TRUE;
}

void GuardThread() {
    while (g_run) {
        int n = 0;
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&n));
        Sleep(750);
    }
}

bool LaunchAndWait(const StartOptions& job) {
    if (job.command.empty()) {
        LogError(L"[session] empty command");
        return false;
    }
    std::wstring cmd = job.command;
    std::wstring work = job.workdir;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             work.empty() ? nullptr : work.c_str(), &si, &pi);
    if (!ok) {
        LogError(L"[session] CreateProcess failed: " + FormatWin32(GetLastError()) + L" cmd=" + cmd);
        return false;
    }
    LogInfo(L"[session] started pid=" + std::to_wstring(pi.dwProcessId) + L" " + cmd);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    LogInfo(L"[session] process exited " + std::to_wstring(code));
    return true;
}

}  // namespace

int CmdSessionEntry(const std::wstring& job_path, DWORD expect_session) {
    FreeConsole();
    LogInit();
    LogInfo(L"[session] helper starting");

    {
        DWORD mine = CurrentSessionId();
        LogInfo(L"[session] session=" + std::to_wstring(mine) + L" expect=" +
                std::to_wstring(expect_session));
        // WTSGetChildSessionId only works in the parent session. Identify the child
        // by the session id the host verified when it spawned this process.
        if (expect_session != kNoSession && mine != expect_session) {
            LogWarn(L"[session] not in the expected child session; exiting");
            return 0;
        }
        if (mine == 0) {
            LogWarn(L"[session] refusing to run in session 0");
            return 0;
        }
    }

    StartOptions job;
    if (!ReadJobFile(job_path, job)) {
        LogError(L"[session] cannot read job " + job_path);
        return 1;
    }
    DeleteJobFile(job_path);

    std::thread guard(GuardThread);
    bool ok = LaunchAndWait(job);
    g_run = false;
    if (guard.joinable()) guard.join();

    if (!job.desktop && !job.keep_session) {
        LogInfo(L"[session] logging off child session");
        WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, FALSE);
    }
    return ok ? 0 : 1;
}
