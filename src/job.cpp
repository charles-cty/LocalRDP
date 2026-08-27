#include "common.hpp"

#include <sstream>

std::wstring WriteJobFile(const StartOptions& opt) {
    std::wstring dir = TempDir();
    wchar_t name[MAX_PATH];
    swprintf_s(name, L"lrdp-%lu-%lu.job", GetCurrentProcessId(), GetTickCount());
    std::wstring path = dir + L"\\" + name;

    std::string body;
    body += "LOCALRDP-JOB-1\n";
    body += "workdir=";
    body += WideToUtf8(opt.workdir);
    body += "\ncommand=";
    body += WideToUtf8(opt.command);
    body += "\nkeep_session=";
    body += opt.keep_session ? "1\n" : "0\n";
    body += "desktop=";
    body += opt.desktop ? "1\n" : "0\n";
    if (!WriteUtf8File(path, body)) {
        LogError(L"Failed to write job file " + path + L": " + FormatWin32(GetLastError()));
        return L"";
    }
    return path;
}

bool ReadJobFile(const std::wstring& path, StartOptions& opt) {
    std::string raw;
    if (!ReadUtf8File(path, raw)) return false;
    std::istringstream in(raw);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!header) {
            if (line != "LOCALRDP-JOB-1") return false;
            header = true;
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "workdir") opt.workdir = Utf8ToWide(val);
        else if (key == "command") opt.command = Utf8ToWide(val);
        else if (key == "keep_session") opt.keep_session = val == "1";
        else if (key == "desktop") opt.desktop = val == "1";
    }
    return header;
}

void DeleteJobFile(const std::wstring& path) {
    if (!path.empty()) DeleteFileW(path.c_str());
}
