#include "common.hpp"

#include <winsvc.h>

namespace {

std::wstring ReadRegSz(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buf[256]{};
    DWORD type = 0;
    DWORD cb = sizeof(buf);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buf, &cb) !=
        ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

DWORD ReadRegDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD def) {
    DWORD v = def;
    DWORD cb = sizeof(v);
    DWORD type = 0;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, &type, &v, &cb) != ERROR_SUCCESS) {
        return def;
    }
    return v;
}

bool ServiceRunning(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    bool running = false;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                             sizeof(ssp), &needed)) {
        running = ssp.dwCurrentState == SERVICE_RUNNING;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

bool ClsidRegistered(const wchar_t* clsid) {
    std::wstring key = std::wstring(L"CLSID\\") + clsid;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(h);
    return true;
}

using WinStationCreateChildSessionTransportFn = int(WINAPI*)(wchar_t*, int);

HRESULT ProbeTransport(std::wstring& pipe) {
    pipe.clear();
    HMODULE m = LoadLibraryW(L"winsta.dll");
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    auto fn = reinterpret_cast<WinStationCreateChildSessionTransportFn>(
        GetProcAddress(m, "WinStationCreateChildSessionTransport"));
    if (!fn) {
        FreeLibrary(m);
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    wchar_t buf[0x80]{};
    int status = fn(buf, 0x80);
    if (status == 0) pipe = buf;
    FreeLibrary(m);
    return static_cast<HRESULT>(status);
}

}  // namespace

bool ChildSessionsEnabled() {
    BOOL enabled = FALSE;
    return WTSIsChildSessionsEnabled(&enabled) && enabled;
}

DWORD GetChildSessionId() {
    ULONG id = kNoSession;
    if (!WTSGetChildSessionId(&id)) return kNoSession;
    return id;
}

bool InsideChildSession() {
    DWORD mine = CurrentSessionId();
    DWORD child = GetChildSessionId();
    return child != kNoSession && child != 0 && child == mine;
}

bool HasActiveChildSession() {
    DWORD id = GetChildSessionId();
    return id != kNoSession && id != 0;
}

std::wstring DescribeTransport(HRESULT status) {
    switch (static_cast<ULONG>(status)) {
        case 0:
            return L"ready";
        case 0x800706BAu:
            return L"RPC server unavailable — child-session transport is not up";
        case 0x80070005u:
            return L"access denied";
        default: {
            wchar_t buf[64];
            swprintf_s(buf, L"error %s", FormatHr(status).c_str());
            return buf;
        }
    }
}

EnvReport QueryEnvironment() {
    EnvReport r;
    r.edition_id = ReadRegSz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             L"EditionID");
    r.build_number =
        ReadRegSz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                  L"CurrentBuild");
    r.home_edition = _wcsnicmp(r.edition_id.c_str(), L"Core", 4) == 0;
    r.child_sessions_enabled = ChildSessionsEnabled();
    r.term_service_running = ServiceRunning(L"TermService");
    r.rdp_listener_enabled =
        ReadRegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
                     L"fDenyTSConnections", 1) == 0;
    r.rdp_control_registered = ClsidRegistered(L"{A0C63C30-F08D-4AB4-907C-34905D770C7D}") ||
                               ClsidRegistered(L"{8B918B82-7985-4C24-89DF-C33AD2BBFBCD}");
    r.inside_child_session = InsideChildSession();
    r.elevated = IsElevated();
    r.child_session_id = GetChildSessionId();
    r.current_session_id = CurrentSessionId();
    r.transport_status = ProbeTransport(r.transport_pipe);

    if (r.home_edition) {
        r.next_action =
            L"This Windows edition has no Remote Desktop server component. Use Pro, Enterprise, Education, or Server.";
    } else if (r.inside_child_session) {
        r.next_action = L"Run this tool from the parent session, not from inside the child session.";
    } else if (!r.rdp_control_registered) {
        r.next_action = L"The Remote Desktop ActiveX control is not registered (mstscax.dll).";
    } else if (!r.child_sessions_enabled) {
        r.next_action =
            L"Child sessions are off. `localrdp start` will enable them (one administrator prompt if needed).";
    } else if (FAILED(r.transport_status)) {
        r.next_action = L"Child-session transport is not available. Try `localrdp start` anyway.";
    }
    return r;
}

void PrintStatus(const EnvReport& r) {
    auto yn = [](bool v) { return v ? L"yes" : L"no"; };
    auto line = [&](const wchar_t* k, const std::wstring& v) {
        std::wstring s = k;
        s.append(L" = ");
        s.append(v);
        PrintOut(s);
    };
    line(L"Version              ", L"LocalRDP " LOCALRDP_VERSION_STRING);
    line(L"EditionID            ", r.edition_id + L" (build " + r.build_number + L")");
    line(L"HomeEdition          ", yn(r.home_edition));
    line(L"Elevated             ", yn(r.elevated));
    line(L"ChildSessionsEnabled ", yn(r.child_sessions_enabled));
    line(L"TermServiceRunning   ", yn(r.term_service_running));
    line(L"RdpListenerEnabled   ", yn(r.rdp_listener_enabled));
    line(L"RdpControlRegistered ", yn(r.rdp_control_registered));
    line(L"InsideChildSession   ", yn(r.inside_child_session));
    line(L"CurrentSessionId     ", std::to_wstring(r.current_session_id));
    if (r.child_session_id == kNoSession) {
        line(L"ChildSessionId       ", L"(none)");
    } else {
        line(L"ChildSessionId       ", std::to_wstring(r.child_session_id));
    }
    wchar_t hrbuf[16];
    swprintf_s(hrbuf, L"0x%08X", static_cast<unsigned>(r.transport_status));
    line(L"Transport            ", std::wstring(hrbuf) + L" (" + DescribeTransport(r.transport_status) + L")");
    if (!r.transport_pipe.empty()) line(L"TransportPipe        ", r.transport_pipe);
    line(L"Ready                ", yn(r.ready()));
    if (!r.next_action.empty()) line(L"NextAction           ", r.next_action);
}
