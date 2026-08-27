#include "common.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>

std::wstring GetExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return L"";
        if (n < path.size() - 1) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring GetExeDir() {
    std::wstring p = GetExePath();
    auto slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

DWORD CurrentSessionId() {
    DWORD id = kNoSession;
    ProcessIdToSessionId(GetCurrentProcessId(), &id);
    return id;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD n = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &n);
    CloseHandle(token);
    return ok && elev.TokenIsElevated;
}

bool EnablePrivilege(std::wstring_view name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    std::wstring n(name);
    if (!LookupPrivilegeValueW(nullptr, n.c_str(), &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

std::wstring CommandLineTail() {
    const wchar_t* p = GetCommandLineW();
    if (!p || !*p) return L"";
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p == L'"') ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

int RelaunchElevated(const std::wstring& args) {
    std::wstring exe = GetExePath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) return static_cast<int>(ExitCode::Cancelled);
        PrintErr(L"Failed to elevate: " + FormatWin32(err));
        return static_cast<int>(ExitCode::Error);
    }
    DWORD code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    return static_cast<int>(code);
}

std::wstring GetEnvVar(std::wstring_view name) {
    std::wstring key(name);
    DWORD n = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (n == 0) return L"";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(key.c_str(), v.data(), n);
    if (n == 0) return L"";
    v.resize(n);
    return v;
}

std::wstring LocalAppData() {
    std::wstring v = GetEnvVar(L"LOCALAPPDATA");
    if (!v.empty()) return v;
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return path;
    return TempDir();
}

std::wstring TempDir() {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return L".";
    std::wstring s(path);
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return false;
    if (!EnsureDir(path.substr(0, slash))) return false;
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteUtf8File(const std::wstring& path, std::string_view utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

bool ReadUtf8File(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), n, nullptr,
                        nullptr);
    return u;
}

std::wstring QuoteArg(std::wstring_view arg) {
    bool need = arg.empty();
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"' || c == L'\n') {
            need = true;
            break;
        }
    }
    if (!need) return std::wstring(arg);

    std::wstring out;
    out.push_back(L'"');
    int slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
            continue;
        }
        if (slashes) {
            out.append(slashes, L'\\');
            slashes = 0;
        }
        out.push_back(c);
    }
    if (slashes) out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const std::vector<std::wstring>& args) {
    std::wstring s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s.push_back(L' ');
        s += QuoteArg(args[i]);
    }
    return s;
}

std::wstring GetShortPath(const std::wstring& path) {
    DWORD n = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (n == 0) return path;
    std::wstring s(n, L'\0');
    n = GetShortPathNameW(path.c_str(), s.data(), n);
    if (n == 0) return path;
    s.resize(n);
    return s;
}

RECT PrimaryWorkArea() {
    RECT r{0, 0, 1280, 720};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

UINT WindowDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(hwnd);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(hwnd, dc);
    return dpi ? dpi : 96;
}
