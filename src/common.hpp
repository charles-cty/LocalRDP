#pragma once

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wtsapi32.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef LOCALRDP_VERSION_STRING
#define LOCALRDP_VERSION_STRING "1.0.0"
#endif

inline constexpr DWORD kNoSession = 0xFFFFFFFFu;

enum class ExitCode : int {
    Ok = 0,
    Error = 1,
    NotReady = 2,
    ConnectFailed = 3,
    Cancelled = 4,
    Conflict = 5,
};

struct StartOptions {
    int width = 0;
    int height = 0;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int scale = 0;          // 0 = follow host DPI; otherwise 100–500
    int timeout_sec = 90;
    std::wstring workdir;
    std::wstring title = L"LocalRDP";
    std::wstring user;
    std::wstring password;
    std::wstring command;   // full command line; empty means interactive desktop
    bool minimized = false;
    bool hidden = false;
    bool clipboard = true;
    bool drives = false;  // RDPDR on child sessions can stall Connect(); opt-in via --drives
    bool view_only = false;
    bool keep_session = false;
    bool desktop = false;
    bool fullscreen = false;
};

struct EnvReport {
    std::wstring edition_id;
    std::wstring build_number;
    bool home_edition = false;
    bool child_sessions_enabled = false;
    bool term_service_running = false;
    bool rdp_listener_enabled = false;
    bool rdp_control_registered = false;
    bool inside_child_session = false;
    bool elevated = false;
    DWORD child_session_id = kNoSession;
    DWORD current_session_id = kNoSession;
    HRESULT transport_status = E_FAIL;
    std::wstring transport_pipe;
    std::wstring next_action;

    bool blocked() const {
        return home_edition || !rdp_control_registered || inside_child_session;
    }
    bool ready() const { return !blocked(); }
};

// log.cpp
void LogInit();
void LogInfo(std::wstring_view msg);
void LogWarn(std::wstring_view msg);
void LogError(std::wstring_view msg);
void PrintOut(std::wstring_view msg);
void PrintErr(std::wstring_view msg);
std::wstring FormatWin32(DWORD err);
std::wstring FormatHr(HRESULT hr);
std::wstring Join(std::wstring_view a, std::wstring_view b);

// win_util.cpp
std::wstring GetExePath();
std::wstring GetExeDir();
DWORD CurrentSessionId();
bool IsElevated();
bool EnablePrivilege(std::wstring_view name);
int RelaunchElevated(const std::wstring& args);
std::wstring CommandLineTail();
std::wstring GetEnvVar(std::wstring_view name);
std::wstring LocalAppData();
std::wstring TempDir();
bool WriteUtf8File(const std::wstring& path, std::string_view utf8);
bool ReadUtf8File(const std::wstring& path, std::string& out);
std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);
std::wstring QuoteArg(std::wstring_view arg);
std::wstring BuildCommandLine(const std::vector<std::wstring>& args);
std::wstring GetShortPath(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
RECT PrimaryWorkArea();
UINT WindowDpi(HWND hwnd);

// dispatch.cpp
HRESULT DispPut(IDispatch* disp, const wchar_t* name, const VARIANT& value);
HRESULT DispGet(IDispatch* disp, const wchar_t* name, VARIANT* value);
HRESULT DispGet1(IDispatch* disp, const wchar_t* name, const VARIANT& arg, VARIANT* value);
HRESULT DispCall0(IDispatch* disp, const wchar_t* name);
HRESULT DispCall1(IDispatch* disp, const wchar_t* name, const VARIANT& a, VARIANT* ret);
HRESULT DispCall2(IDispatch* disp, const wchar_t* name, const VARIANT& a, const VARIANT& b, VARIANT* ret);
HRESULT DispCallN(IDispatch* disp, const wchar_t* name, const VARIANT* in_order, UINT n, VARIANT* ret);
HRESULT DispGetDispatch(IDispatch* disp, const wchar_t* name, IDispatch** out);

// env.cpp
EnvReport QueryEnvironment();
std::wstring DescribeTransport(HRESULT status);
void PrintStatus(const EnvReport& r);
bool ChildSessionsEnabled();
DWORD GetChildSessionId();
bool InsideChildSession();
bool HasActiveChildSession();
int EnsureChildSessionsForStart();

// start.cpp
int CmdStart(const StartOptions& opt);
int CmdStop();

// session_entry.cpp
int CmdSessionEntry(const std::wstring& job_path, DWORD expect_session);

// job.cpp
std::wstring WriteJobFile(const StartOptions& opt);
bool ReadJobFile(const std::wstring& path, StartOptions& opt);
void DeleteJobFile(const std::wstring& path);

// session_launch.cpp
bool LaunchInSession(DWORD session_id, const std::wstring& command, const std::wstring& workdir,
                     DWORD* pid);

// clip_bridge.cpp
int CmdClipBridge(DWORD expect_session, DWORD parent_pid);
void ClipHostStart();
void ClipHostStop();
std::wstring ClipBridgeCommand(DWORD expect_session);

// rdp_host.cpp
int RunRdpSession(const StartOptions& opt, const std::wstring& job_path);
