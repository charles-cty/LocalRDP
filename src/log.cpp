#include "common.hpp"

#include <fcntl.h>
#include <io.h>

#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace {

std::mutex g_mu;
std::wstring g_log_path;
bool g_inited = false;

std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void WriteConsoleUtf16(HANDLE h, std::wstring_view msg) {
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD written = 0;
        WriteConsoleW(h, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
        const wchar_t nl[] = L"\r\n";
        WriteConsoleW(h, nl, 2, &written, nullptr);
        return;
    }
    std::string utf8 = WideToUtf8(msg);
    utf8.append("\r\n");
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void AppendLog(std::wstring_view level, std::wstring_view msg) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_log_path.empty()) return;
    std::ofstream f(g_log_path, std::ios::app | std::ios::binary);
    if (!f) return;
    std::string line = WideToUtf8(Timestamp());
    line += " [";
    line += WideToUtf8(level);
    line += "] ";
    line += WideToUtf8(msg);
    line += "\n";
    f.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace

void LogInit() {
    if (g_inited) return;
    g_inited = true;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::wstring dir = LocalAppData() + L"\\LocalRDP";
    EnsureDir(dir);
    g_log_path = dir + L"\\localrdp.log";
}

void PrintOut(std::wstring_view msg) {
    WriteConsoleUtf16(GetStdHandle(STD_OUTPUT_HANDLE), msg);
}

void PrintErr(std::wstring_view msg) {
    WriteConsoleUtf16(GetStdHandle(STD_ERROR_HANDLE), msg);
}

void LogInfo(std::wstring_view msg) {
    LogInit();
    AppendLog(L"INFO", msg);
}

void LogWarn(std::wstring_view msg) {
    LogInit();
    AppendLog(L"WARN", msg);
}

void LogError(std::wstring_view msg) {
    LogInit();
    AppendLog(L"ERROR", msg);
}

std::wstring FormatWin32(DWORD err) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg = n && buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) {
        msg.pop_back();
    }
    wchar_t code[32];
    swprintf_s(code, L"0x%08X", err);
    if (msg.empty()) return code;
    return std::wstring(code) + L" (" + msg + L")";
}

std::wstring FormatHr(HRESULT hr) {
    return FormatWin32(static_cast<DWORD>(hr));
}

std::wstring Join(std::wstring_view a, std::wstring_view b) {
    std::wstring s;
    s.reserve(a.size() + b.size());
    s.append(a);
    s.append(b);
    return s;
}
