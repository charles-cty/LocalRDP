#include "common.hpp"
#include "rdp_events.hpp"

#include <atlbase.h>
#include <atlcom.h>
#include <atlwin.h>
#include <atlhost.h>
#include <commctrl.h>
#include <oleidl.h>

#include <algorithm>
#include <atomic>
#include <vector>

#pragma comment(lib, "atls.lib")

class LocalRdpAtlModule : public ATL::CAtlExeModuleT<LocalRdpAtlModule> {};
LocalRdpAtlModule g_atl_module;

namespace {

constexpr wchar_t kWndClass[] = L"LocalRDP.Host";
constexpr UINT WM_RDP_DISCONNECTED = WM_APP + 1;
constexpr UINT_PTR kTimerWatchdog = 1;
constexpr UINT_PTR kTimerLaunch = 2;
constexpr UINT_PTR kTimerQuit = 3;
constexpr UINT_PTR kTimerResize = 4;
constexpr UINT_PTR kTimerDisplay = 5;

const CLSID kRdpClsids[] = {
    {0xa0c63c30, 0xf08d, 0x4ab4, {0x90, 0x7c, 0x34, 0x90, 0x5d, 0x77, 0x0c, 0x7d}},  // v11 / Client10 NSF
    {0x8b918b82, 0x7985, 0x4c24, {0x89, 0xdf, 0xc3, 0x3a, 0xd2, 0xbb, 0xfb, 0xcd}},  // v10 / Client9 NSF
    {0xa3bc03a0, 0x041d, 0x42e3, {0xad, 0x22, 0x88, 0x2b, 0x78, 0x65, 0xc9, 0xc5}},  // v9 / Client8 NSF
};

HWND g_host_hwnd = nullptr;
std::atomic<bool> g_stop{false};

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        if (g_host_hwnd) PostMessageW(g_host_hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }
    return FALSE;
}

VARIANT VarI4(long v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_I4;
    x.lVal = v;
    return x;
}
VARIANT VarBool(bool v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_BOOL;
    x.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    return x;
}
VARIANT VarBstr(const wchar_t* s) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_BSTR;
    x.bstrVal = SysAllocString(s ? s : L"");
    return x;
}

void TryPut(IDispatch* d, const wchar_t* name, VARIANT v) {
    HRESULT hr = DispPut(d, name, v);
    if (FAILED(hr)) {
        LogWarn(std::wstring(L"property ") + name + L" failed: " + FormatHr(hr));
    }
    VariantClear(&v);
}

IDispatch* GetFirstDispatch(IDispatch* parent, const wchar_t* const* names) {
    for (int i = 0; names[i]; ++i) {
        IDispatch* child = nullptr;
        if (SUCCEEDED(DispGetDispatch(parent, names[i], &child)) && child) {
            LogInfo(std::wstring(L"using ") + names[i]);
            return child;
        }
    }
    return nullptr;
}

void TryPutOn(IDispatch* obj, const wchar_t* name, VARIANT v) {
    if (!obj) {
        VariantClear(&v);
        return;
    }
    TryPut(obj, name, v);
}

std::wstring ClsidString(const CLSID& c) {
    LPOLESTR s = nullptr;
    StringFromCLSID(c, &s);
    std::wstring w = s ? s : L"";
    if (s) CoTaskMemFree(s);
    return w;
}

bool PutExtendedBool(IUnknown* unk, const wchar_t* name, bool value) {
    IMsRdpExtendedSettings* ext = nullptr;
    HRESULT hr = unk->QueryInterface(__uuidof(IMsRdpExtendedSettings), reinterpret_cast<void**>(&ext));
    if (FAILED(hr) || !ext) {
        LogWarn(L"IMsRdpExtendedSettings missing: " + FormatHr(hr));
        return false;
    }
    VARIANT v = VarBool(value);
    BSTR b = SysAllocString(name);
    hr = ext->put_Property(b, &v);
    SysFreeString(b);
    VariantClear(&v);
    ext->Release();
    if (FAILED(hr)) {
        LogWarn(std::wstring(L"extended ") + name + L" failed: " + FormatHr(hr));
        return false;
    }
    return true;
}

bool PutExtendedI4(IUnknown* unk, const wchar_t* name, long value) {
    IMsRdpExtendedSettings* ext = nullptr;
    HRESULT hr = unk->QueryInterface(__uuidof(IMsRdpExtendedSettings), reinterpret_cast<void**>(&ext));
    if (FAILED(hr) || !ext) return false;
    VARIANT v = VarI4(value);
    BSTR b = SysAllocString(name);
    hr = ext->put_Property(b, &v);
    SysFreeString(b);
    VariantClear(&v);
    ext->Release();
    return SUCCEEDED(hr);
}

std::wstring DescribeDisconnect(int reason, int extended) {
    switch (extended) {
        case 5:
            return L"The child session was taken over by another connection (only one is allowed).";
        case 2:
        case 12:
            return L"The child session was logged off.";
    }
    switch (reason) {
        case 0x1:
            return L"Disconnected locally.";
        case 0x2:
            return L"Logged off inside the child session.";
        case 0x3:
            return L"Session ended by the system.";
        case 0x108:
            return L"Connection timed out.";
        case 0x204:
            return L"Cannot connect: the child-session transport is not available.";
        case 0x406:
            return L"Invalid security data.";
        case 0x706:
            return L"Loopback certificate check failed.";
        case 0x807:
            return L"Logon failed: credentials rejected.";
        case 0x904:
            return L"Connection closed.";
        case 0x1607:
        case 0x1707:
            return L"Credential delegation is blocked by policy; cannot auto-logon.";
    }
    wchar_t buf[64];
    swprintf_s(buf, L"disconnect reason=0x%X extended=%d", reason, extended);
    return buf;
}

struct HostState {
    const StartOptions* opt = nullptr;
    HWND hwnd = nullptr;
    HWND ax_hwnd = nullptr;
    IUnknown* control = nullptr;
    IDispatch* disp = nullptr;
    IOleInPlaceActiveObject* ipao = nullptr;
    RdpEventSink* sink = nullptr;
    bool ever_connected = false;
    bool logged_in = false;
    bool closing = false;
    bool failed = false;
    bool fullscreen = false;
    int disc_reason = 0;
    int ext_reason = 0;
    UINT_PTR watchdog = 0;
    int ticks = 0;
    std::wstring job_path;
    bool launched = false;
    int applied_w = 0;
    int applied_h = 0;
    int applied_scale = 0;
    WINDOWPLACEMENT restore{};
    DWORD restore_style = 0;
    DWORD restore_ex = 0;
};

RECT MonitorRect(HWND hwnd, bool work_area) {
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) return work_area ? mi.rcWork : mi.rcMonitor;
    return PrimaryWorkArea();
}

void ClientDesktopSize(HWND hwnd, int* w, int* h) {
    RECT rc{};
    if (hwnd) GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    *w = (std::max)(200, cw & ~1);
    *h = (std::max)(200, ch);
}

void ResolveScale(HWND hwnd, const StartOptions* opt, int* desktop, int* device) {
    int pct = opt ? opt->scale : 0;
    if (pct <= 0) {
        UINT dpi = WindowDpi(hwnd);
        if (dpi == 0) dpi = 96;
        pct = static_cast<int>((dpi * 100 + 48) / 96);
    }
    pct = std::clamp(pct, 100, 500);
    *desktop = pct;
    *device = pct >= 200 ? 180 : (pct >= 125 ? 140 : 100);
}

void ResizeAx(HostState* st) {
    if (!st->hwnd || !st->ax_hwnd) return;
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    MoveWindow(st->ax_hwnd, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void ScheduleDisplaySync(HostState* st, UINT delay_ms) {
    if (!st || !st->hwnd || !st->logged_in || st->closing) return;
    SetTimer(st->hwnd, kTimerResize, delay_ms, nullptr);
}

bool ApplyDisplay(HostState* st) {
    if (!st || !st->disp || !st->hwnd || !st->logged_in) return false;
    int w = 0, h = 0, ds = 0, dev = 0;
    ClientDesktopSize(st->hwnd, &w, &h);
    ResolveScale(st->hwnd, st->opt, &ds, &dev);
    if (w == st->applied_w && h == st->applied_h && ds == st->applied_scale) return true;

    UINT dpi = WindowDpi(st->hwnd);
    if (dpi == 0) dpi = 96;
    long phys_w = (std::max)(10, (std::min)(10000, static_cast<int>(w * 254 / (10 * static_cast<int>(dpi)))));
    long phys_h = (std::max)(10, (std::min)(10000, static_cast<int>(h * 254 / (10 * static_cast<int>(dpi)))));

    auto call_update = [&](long pw, long ph) {
        VARIANT args[7] = {VarI4(w), VarI4(h), VarI4(pw), VarI4(ph), VarI4(0), VarI4(ds), VarI4(dev)};
        return DispCallN(st->disp, L"UpdateSessionDisplaySettings", args, 7, nullptr);
    };
    HRESULT hr = call_update(phys_w, phys_h);
    if (FAILED(hr)) hr = call_update(0, 0);
    if (FAILED(hr)) {
        LogWarn(L"UpdateSessionDisplaySettings failed: " + FormatHr(hr));
        VARIANT ret;
        VariantInit(&ret);
        VARIANT recon[2] = {VarI4(w), VarI4(h)};
        hr = DispCallN(st->disp, L"Reconnect", recon, 2, &ret);
        long status = (ret.vt == VT_I4) ? ret.lVal : (ret.vt == VT_UI4 ? static_cast<long>(ret.ulVal) : -1);
        VariantClear(&ret);
        if (FAILED(hr) || status != 0) {
            LogWarn(L"Reconnect display fallback failed hr=" + FormatHr(hr) + L" status=" +
                    std::to_wstring(status));
            return false;
        }
        LogInfo(L"Display updated via Reconnect " + std::to_wstring(w) + L"x" + std::to_wstring(h));
    } else {
        LogInfo(L"Display updated " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L" @" +
                std::to_wstring(ds) + L"% dpi=" + std::to_wstring(dpi));
    }
    st->applied_w = w;
    st->applied_h = h;
    st->applied_scale = ds;
    return true;
}

void SetControlFullScreen(HostState* st, bool on) {
    if (!st || !st->disp) return;
    TryPut(st->disp, L"FullScreen", VarBool(on));
}

void EnterFullscreen(HostState* st) {
    if (!st || !st->hwnd || st->fullscreen || (st->opt && st->opt->hidden)) return;
    st->restore.length = sizeof(st->restore);
    GetWindowPlacement(st->hwnd, &st->restore);
    st->restore_style = static_cast<DWORD>(GetWindowLongPtrW(st->hwnd, GWL_STYLE));
    st->restore_ex = static_cast<DWORD>(GetWindowLongPtrW(st->hwnd, GWL_EXSTYLE));
    RECT mon = MonitorRect(st->hwnd, false);
    SetWindowLongPtrW(st->hwnd, GWL_STYLE,
                      WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(st->hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(st->hwnd, HWND_TOP, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    st->fullscreen = true;
    SetControlFullScreen(st, true);
    ResizeAx(st);
    ScheduleDisplaySync(st, 200);
}

void LeaveFullscreen(HostState* st) {
    if (!st || !st->hwnd || !st->fullscreen) return;
    DWORD style = st->restore_style ? st->restore_style
                                    : (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(st->hwnd, GWL_STYLE, static_cast<LONG_PTR>(style | WS_VISIBLE));
    if (st->restore_ex) SetWindowLongPtrW(st->hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(st->restore_ex));
    st->fullscreen = false;
    SetControlFullScreen(st, false);
    if (st->restore.length) SetWindowPlacement(st->hwnd, &st->restore);
    ResizeAx(st);
    ScheduleDisplaySync(st, 200);
}

void ToggleFullscreen(HostState* st) {
    if (!st) return;
    if (st->fullscreen) LeaveFullscreen(st);
    else EnterFullscreen(st);
}

void RequestClose(HostState* st) {
    if (!st || st->closing) return;
    st->closing = true;
    if (st->hwnd) {
        SetTimer(st->hwnd, kTimerQuit, 500, nullptr);
    }
    PostQuitMessage(0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<HostState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_SIZE:
            if (st) {
                ResizeAx(st);
                if (wParam != SIZE_MINIMIZED) ScheduleDisplaySync(st, 400);
            }
            return 0;
        case WM_DPICHANGED:
            if (st) {
                auto* rec = reinterpret_cast<RECT*>(lParam);
                if (rec && !st->fullscreen) {
                    SetWindowPos(hwnd, nullptr, rec->left, rec->top, rec->right - rec->left,
                                 rec->bottom - rec->top, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                ResizeAx(st);
                ScheduleDisplaySync(st, 200);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            mm->ptMinTrackSize.x = 480;
            mm->ptMinTrackSize.y = 320;
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (st && wParam == VK_F11) {
                ToggleFullscreen(st);
                return 0;
            }
            break;
        case WM_RDP_DISCONNECTED:
            if (st) {
                st->disc_reason = static_cast<int>(wParam);
                if (!st->ever_connected) st->failed = true;
                // Do not DestroyWindow here: tearing down MsRdpClient from its
                // own disconnect callback (or immediately after) can hang.
                RequestClose(st);
            }
            return 0;
        case WM_TIMER:
            if (st && wParam == kTimerLaunch) {
                KillTimer(hwnd, kTimerLaunch);
                if (st->launched || !st->opt) return 0;
                st->launched = true;
                if (st->job_path.empty()) return 0;
                DWORD sid = GetChildSessionId();
                if (sid == kNoSession || sid == 0) {
                    PrintErr(L"Logon completed but no child session id is available.");
                    return 0;
                }
                std::wstring cmd = QuoteArg(GetExePath()) + L" --session-entry --job " +
                                   QuoteArg(st->job_path) + L" --expect-session " +
                                   std::to_wstring(sid);
                DWORD pid = 0;
                if (!LaunchInSession(sid, cmd, L"", &pid)) {
                    PrintErr(L"Connected, but failed to start the program inside the child session.");
                    LogError(L"LaunchInSession failed cmd=" + cmd);
                    if (!st->opt->desktop) {
                        st->failed = true;
                        RequestClose(st);
                    }
                } else {
                    PrintOut(L"Started program in child session pid=" + std::to_wstring(pid));
                }
                return 0;
            }
            if (st && (wParam == kTimerResize || wParam == kTimerDisplay)) {
                KillTimer(hwnd, static_cast<UINT_PTR>(wParam));
                ApplyDisplay(st);
                return 0;
            }
            if (st && wParam == kTimerQuit) {
                KillTimer(hwnd, kTimerQuit);
                PostQuitMessage(st->failed ? 1 : 0);
                return 0;
            }
            if (st && wParam == kTimerWatchdog) {
                ++st->ticks;
                if (st->ever_connected) {
                    KillTimer(hwnd, kTimerWatchdog);
                    st->watchdog = 0;
                    break;
                }
                int timeout = st->opt ? st->opt->timeout_sec : 90;
                if (st->disp && (st->ticks % 5) == 0) {
                    VARIANT cv;
                    VariantInit(&cv);
                    if (SUCCEEDED(DispGet(st->disp, L"Connected", &cv))) {
                        LogInfo(L"watchdog Connected=" + std::to_wstring(cv.lVal));
                        VariantClear(&cv);
                    }
                }
                if (st->ticks >= timeout) {
                    LogError(L"Connect timed out");
                    PrintErr(L"Timed out waiting for the child session.");
                    PrintErr(L"Typical causes: child sessions could not be enabled, account has no password, or this process is itself in a Remote Desktop session.");
                    st->failed = true;
                    KillTimer(hwnd, kTimerWatchdog);
                    st->watchdog = 0;
                    PostQuitMessage(1);
                }
            }
            return 0;
        case WM_CLOSE:
            if (st) {
                if (st->disp && st->ever_connected && !st->closing) {
                    st->closing = true;
                    DispCall0(st->disp, L"Disconnect");
                    SetTimer(hwnd, kTimerQuit, 2000, nullptr);
                } else {
                    RequestClose(st);
                }
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            g_host_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnConnecting(void* ctx) {
    auto* st = static_cast<HostState*>(ctx);
    PrintOut(L"Connecting to local child session...");
    LogInfo(L"OnConnecting");
    (void)st;
}

void OnConnected(void* ctx) {
    auto* st = static_cast<HostState*>(ctx);
    st->ever_connected = true;
    PrintOut(L"Connected.");
    LogInfo(L"OnConnected");
}

void OnLogin(void* ctx) {
    auto* st = static_cast<HostState*>(ctx);
    st->logged_in = true;
    DWORD id = GetChildSessionId();
    std::wstring msg = L"Logon complete.";
    if (id != kNoSession) msg += L" Child session id=" + std::to_wstring(id);
    PrintOut(msg);
    LogInfo(msg);
    st->applied_w = 0;
    st->applied_h = 0;
    st->applied_scale = 0;
    // Wait for Explorer, then spawn the user program into this session.
    if (st->hwnd) {
        SetTimer(st->hwnd, kTimerDisplay, 800, nullptr);
        if (!st->job_path.empty()) SetTimer(st->hwnd, kTimerLaunch, 1500, nullptr);
    }
}

void OnGoFullscreen(void* ctx) { EnterFullscreen(static_cast<HostState*>(ctx)); }
void OnLeaveFullscreen(void* ctx) { LeaveFullscreen(static_cast<HostState*>(ctx)); }
void OnContainerMinimize(void* ctx) {
    auto* st = static_cast<HostState*>(ctx);
    if (st && st->hwnd) ShowWindow(st->hwnd, SW_MINIMIZE);
}

void OnDisconnected(void* ctx, int reason) {
    auto* st = static_cast<HostState*>(ctx);
    st->disc_reason = reason;
    if (st->disp) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(DispGet(st->disp, L"ExtendedDisconnectReason", &v))) {
            if (v.vt == VT_I4) st->ext_reason = v.lVal;
            VariantClear(&v);
        }
    }
    std::wstring d = DescribeDisconnect(reason, st->ext_reason);
    LogInfo(L"OnDisconnected " + d);
    if (st->hwnd) PostMessageW(st->hwnd, WM_RDP_DISCONNECTED, static_cast<WPARAM>(reason), 0);
}

void OnFatal(void* ctx, int code) {
    auto* st = static_cast<HostState*>(ctx);
    LogError(L"RDP fatal error " + std::to_wstring(code));
    PrintErr(L"RDP fatal error " + std::to_wstring(code));
    st->failed = true;
}

bool ConfigureControl(HostState* st) {
    IDispatch* d = st->disp;
    const StartOptions& opt = *st->opt;

    int desk_w = 0, desk_h = 0;
    ClientDesktopSize(st->hwnd, &desk_w, &desk_h);
    TryPut(d, L"Server", VarBstr(L"localhost"));
    TryPut(d, L"DesktopWidth", VarI4(desk_w));
    TryPut(d, L"DesktopHeight", VarI4(desk_h));
    TryPut(d, L"ColorDepth", VarI4(32));

    static const wchar_t* kAdv[] = {L"AdvancedSettings9", L"AdvancedSettings8", L"AdvancedSettings7",
                                    L"AdvancedSettings6", L"AdvancedSettings2", L"AdvancedSettings",
                                    nullptr};
    static const wchar_t* kSec[] = {L"SecuredSettings3", L"SecuredSettings2", L"SecuredSettings",
                                    nullptr};

    IDispatch* adv = GetFirstDispatch(d, kAdv);
    IDispatch* sec = GetFirstDispatch(d, kSec);

    TryPutOn(adv, L"EnableCredSspSupport", VarBool(true));
    TryPutOn(adv, L"AuthenticationLevel", VarI4(0));
    TryPutOn(adv, L"ContainerHandledFullScreen", VarI4(1));
    TryPutOn(adv, L"DisplayConnectionBar", VarBool(false));
    TryPutOn(adv, L"RedirectClipboard", VarBool(opt.clipboard));
    TryPutOn(adv, L"SmartSizing", VarBool(true));
    TryPutOn(adv, L"EnableAutoReconnect", VarBool(false));
    TryPutOn(adv, L"MaxReconnectAttempts", VarI4(0));
    TryPutOn(adv, L"PerformanceFlags", VarI4(0x00000190));
    TryPutOn(adv, L"GrabFocusOnConnect", VarBool(!opt.view_only && !opt.hidden));
    TryPutOn(adv, L"allowBackgroundInput", VarI4(0));
    TryPutOn(adv, L"NegotiateSecurityLayer", VarBool(true));
    TryPutOn(d, L"PromptForCredentials", VarBool(false));
    {
        // IMsRdpClientNonScriptable3 / 5 live on the OCX but not on the default IDispatch.
        static const IID kNs3 = {0xb3378d90, 0x0728, 0x45c7, {0x8e, 0xd7, 0xb6, 0x15, 0x9f, 0xb9, 0x22, 0x19}};
        static const IID kNs5 = {0x4f6996d5, 0xd7b1, 0x412c, {0xb0, 0xff, 0x06, 0x37, 0x18, 0x56, 0x69, 0x07}};
        auto put_ns = [&](const IID& iid, const wchar_t* name, VARIANT v) {
            IUnknown* p = nullptr;
            if (FAILED(st->control->QueryInterface(iid, reinterpret_cast<void**>(&p))) || !p) {
                VariantClear(&v);
                return;
            }
            IDispatch* di = nullptr;
            if (SUCCEEDED(p->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&di))) && di) {
                TryPut(di, name, v);
                di->Release();
            } else {
                VariantClear(&v);
            }
            p->Release();
        };
        put_ns(kNs3, L"PromptForCredentials", VarBool(false));
        put_ns(kNs5, L"AllowPromptingForCredentials", VarBool(false));
    }

    if (!opt.user.empty()) {
        TryPut(d, L"UserName", VarBstr(opt.user.c_str()));
        if (!opt.password.empty()) {
            TryPutOn(adv, L"ClearTextPassword", VarBstr(opt.password.c_str()));
        }
    }

    TryPutOn(sec, L"KeyboardHookMode", VarI4(1));
    if (adv) adv->Release();
    if (sec) sec->Release();

    if (!PutExtendedBool(st->control, L"ConnectToChildSession", true)) {
        PrintErr(L"Failed to set ConnectToChildSession. Use the NotSafeForScripting RDP control.");
        return false;
    }
    {
        IMsRdpExtendedSettings* ext = nullptr;
        if (SUCCEEDED(st->control->QueryInterface(__uuidof(IMsRdpExtendedSettings),
                                                  reinterpret_cast<void**>(&ext))) &&
            ext) {
            VARIANT got;
            VariantInit(&got);
            BSTR b = SysAllocString(L"ConnectToChildSession");
            HRESULT ghr = ext->get_Property(b, &got);
            SysFreeString(b);
            wchar_t buf[80];
            swprintf_s(buf, L"ConnectToChildSession get hr=0x%08X vt=%u bool=%d",
                       static_cast<unsigned>(ghr), got.vt, got.boolVal);
            LogInfo(buf);
            VariantClear(&got);
            ext->Release();
        }
    }
    PutExtendedBool(st->control, L"EnableHardwareMode", true);
    PutExtendedBool(st->control, L"EnableFrameBufferRedirection", true);

    int ds = 0, device = 0;
    ResolveScale(st->hwnd, &opt, &ds, &device);
    PutExtendedI4(st->control, L"DesktopScaleFactor", ds);
    PutExtendedI4(st->control, L"DeviceScaleFactor", device);
    st->applied_w = desk_w;
    st->applied_h = desk_h;
    st->applied_scale = ds;
    LogInfo(L"Initial desktop " + std::to_wstring(desk_w) + L"x" + std::to_wstring(desk_h) + L" @" +
            std::to_wstring(ds) + L"% dpi=" + std::to_wstring(WindowDpi(st->hwnd)));
    return true;
}

}  // namespace

int RunRdpSession(const StartOptions& opt_in, const std::wstring& job_path) {
    StartOptions opt = opt_in;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        PrintErr(L"OleInitialize failed: " + FormatHr(hr));
        return static_cast<int>(ExitCode::Error);
    }
    const bool uninit = (hr == S_OK);

    if (!ATL::AtlAxWinInit()) {
        PrintErr(L"AtlAxWinInit failed.");
        if (uninit) OleUninitialize();
        return static_cast<int>(ExitCode::Error);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    HostState st;
    st.opt = &opt;
    st.job_path = job_path;

    DWORD ex = WS_EX_APPWINDOW;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT wa = MonitorRect(nullptr, true);
    RECT mon = MonitorRect(nullptr, false);
    int client_w = opt.width;
    int client_h = opt.height;
    if (client_w < 200 || client_h < 200) {
        client_w = (std::max)(800, static_cast<int>(wa.right - wa.left) * 5 / 6);
        client_h = (std::max)(600, static_cast<int>(wa.bottom - wa.top) * 5 / 6);
    }
    client_w &= ~1;

    int x = opt.x, y = opt.y, ww = client_w, wh = client_h;
    if (opt.hidden) {
        ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        x = 0;
        y = 0;
        ww = client_w;
        wh = client_h;
    } else if (opt.fullscreen) {
        style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        x = mon.left;
        y = mon.top;
        ww = mon.right - mon.left;
        wh = mon.bottom - mon.top;
    } else {
        RECT wr{0, 0, client_w, client_h};
        AdjustWindowRectEx(&wr, style, FALSE, ex);
        ww = wr.right - wr.left;
        wh = wr.bottom - wr.top;
        if (x == CW_USEDEFAULT) x = wa.left + (std::max)(0, static_cast<int>(wa.right - wa.left - ww) / 2);
        if (y == CW_USEDEFAULT) y = wa.top + (std::max)(0, static_cast<int>(wa.bottom - wa.top - wh) / 2);
    }

    HWND hwnd = CreateWindowExW(ex, kWndClass, opt.title.c_str(), style, x, y, ww, wh, nullptr,
                                nullptr, GetModuleHandleW(nullptr), &st);
    if (!hwnd) {
        PrintErr(L"CreateWindow failed: " + FormatWin32(GetLastError()));
        if (uninit) OleUninitialize();
        return static_cast<int>(ExitCode::Error);
    }
    st.hwnd = hwnd;
    g_host_hwnd = hwnd;
    if (opt.fullscreen && !opt.hidden) {
        st.fullscreen = true;
        st.restore.length = sizeof(st.restore);
        st.restore.showCmd = SW_SHOWNORMAL;
        RECT wr{0, 0, client_w, client_h};
        AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, FALSE,
                           WS_EX_APPWINDOW);
        int rww = wr.right - wr.left;
        int rwh = wr.bottom - wr.top;
        int rx = wa.left + (std::max)(0, static_cast<int>(wa.right - wa.left - rww) / 2);
        int ry = wa.top + (std::max)(0, static_cast<int>(wa.bottom - wa.top - rwh) / 2);
        st.restore.rcNormalPosition = {rx, ry, rx + rww, ry + rwh};
        st.restore_style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        st.restore_ex = WS_EX_APPWINDOW;
    }
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    RECT rc{};
    GetClientRect(hwnd, &rc);

    IUnknown* control = nullptr;
    HWND ax = nullptr;
    hr = E_FAIL;
    std::wstring used;
    const wchar_t* ax_class = ATL::CAxWindow::GetWndClassName();
    for (const CLSID& clsid : kRdpClsids) {
        used = ClsidString(clsid);
        ax = CreateWindowExW(0, ax_class, used.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                             rc.right, rc.bottom, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!ax) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            continue;
        }
        hr = ATL::AtlAxGetControl(ax, &control);
        if (SUCCEEDED(hr) && control) break;
        if (control) {
            control->Release();
            control = nullptr;
        }
        DestroyWindow(ax);
        ax = nullptr;
    }
    st.ax_hwnd = ax;
    if (FAILED(hr) || !control) {
        PrintErr(L"Failed to create the Remote Desktop ActiveX control: " + FormatHr(hr));
        DestroyWindow(hwnd);
        if (uninit) OleUninitialize();
        return static_cast<int>(ExitCode::Error);
    }
    st.control = control;
    LogInfo(L"RDP control CLSID " + used);

    hr = control->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&st.disp));
    if (FAILED(hr) || !st.disp) {
        PrintErr(L"RDP control has no IDispatch.");
        control->Release();
        DestroyWindow(hwnd);
        if (uninit) OleUninitialize();
        return static_cast<int>(ExitCode::Error);
    }
    control->QueryInterface(IID_IOleInPlaceActiveObject, reinterpret_cast<void**>(&st.ipao));

    RdpCallbacks cb{};
    cb.ctx = &st;
    cb.on_connecting = &OnConnecting;
    cb.on_connected = &OnConnected;
    cb.on_login = &OnLogin;
    cb.on_disconnected = &OnDisconnected;
    cb.on_fatal = &OnFatal;
    cb.on_go_fullscreen = &OnGoFullscreen;
    cb.on_leave_fullscreen = &OnLeaveFullscreen;
    cb.on_container_minimize = &OnContainerMinimize;
    st.sink = new RdpEventSink(cb);
    hr = st.sink->Advise(control);
    if (FAILED(hr)) {
        LogWarn(L"RDP event advise failed, falling back to polling: " + FormatHr(hr));
    }

    if (opt.view_only) EnableWindow(ax, FALSE);

    int show = SW_SHOW;
    if (opt.hidden) show = SW_HIDE;
    else if (opt.minimized) show = SW_SHOWMINNOACTIVE;
    ShowWindow(hwnd, show);
    if (!opt.hidden) UpdateWindow(hwnd);
    {
        MSG pump{};
        while (PeekMessageW(&pump, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pump);
            DispatchMessageW(&pump);
        }
    }

    if (!ConfigureControl(&st)) {
        st.sink->Unadvise();
        st.sink->Release();
        if (st.ipao) st.ipao->Release();
        st.disp->Release();
        control->Release();
        DestroyWindow(hwnd);
        if (uninit) OleUninitialize();
        return static_cast<int>(ExitCode::Error);
    }

    hr = DispCall0(st.disp, L"Connect");
    if (FAILED(hr)) {
        PrintErr(L"Connect() failed: " + FormatHr(hr));
        st.failed = true;
        RequestClose(&st);
    } else {
        st.watchdog = SetTimer(hwnd, kTimerWatchdog, 1000, nullptr);
        PrintOut(L"Child-session connection started. Close the window or press Ctrl+C to disconnect.");
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && msg.wParam == VK_F11) {
            ToggleFullscreen(&st);
            continue;
        }
        if (st.ipao && st.ipao->TranslateAccelerator(&msg) == S_OK) continue;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.watchdog) KillTimer(hwnd, kTimerWatchdog);
    if (st.sink) {
        st.sink->Unadvise();
        st.sink->Release();
        st.sink = nullptr;
    }
    if (st.ipao) {
        st.ipao->Release();
        st.ipao = nullptr;
    }
    if (st.disp) {
        st.disp->Release();
        st.disp = nullptr;
    }
    if (st.control) {
        st.control->Release();
        st.control = nullptr;
    }

    SetConsoleCtrlHandler(ConsoleHandler, FALSE);

    if (!opt.keep_session) {
        DWORD id = GetChildSessionId();
        if (id != kNoSession && id != 0) {
            if (WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, id, FALSE)) {
                LogInfo(L"Logged off child session " + std::to_wstring(id));
            }
        }
    }

    if (uninit) OleUninitialize();

    if (st.failed && !st.ever_connected) {
        PrintErr(DescribeDisconnect(st.disc_reason, st.ext_reason));
        return static_cast<int>(ExitCode::ConnectFailed);
    }
    if (st.ever_connected) {
        PrintOut(DescribeDisconnect(st.disc_reason, st.ext_reason));
    }
    return static_cast<int>(ExitCode::Ok);
}
