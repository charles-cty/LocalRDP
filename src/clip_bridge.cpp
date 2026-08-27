#include "common.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <sstream>
#include <vector>

namespace {

constexpr wchar_t kClipWndClass[] = L"LocalRDP.ClipBridge";
constexpr UINT kTimerPoll = 1;
constexpr UINT kTimerParent = 2;

std::wstring ClipDir() { return LocalAppData() + L"\\LocalRDP"; }
std::wstring ToChildPath() { return ClipDir() + L"\\clip-to-child.txt"; }
std::wstring ToParentPath() { return ClipDir() + L"\\clip-to-parent.txt"; }

std::wstring JoinPaths(const std::vector<std::wstring>& paths) {
    std::wstring s;
    for (const auto& p : paths) {
        if (!s.empty()) s += L'\n';
        s += p;
    }
    return s;
}

bool ParseList(const std::string& raw, uint64_t* seq, std::vector<std::wstring>* paths) {
    *seq = 0;
    paths->clear();
    std::istringstream in(raw);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            first = false;
            if (line.rfind("SEQ=", 0) != 0) return false;
            *seq = static_cast<uint64_t>(_strtoui64(line.c_str() + 4, nullptr, 10));
            continue;
        }
        if (!line.empty()) paths->push_back(Utf8ToWide(line));
    }
    return !first;
}

bool WriteList(const std::wstring& path, uint64_t seq, const std::vector<std::wstring>& paths) {
    EnsureDir(ClipDir());
    std::string body = "SEQ=" + std::to_string(seq) + "\n";
    for (const auto& p : paths) {
        body += WideToUtf8(p);
        body += "\n";
    }
    return WriteUtf8File(path, body);
}

bool ReadHdrop(std::vector<std::wstring>* paths) {
    paths->clear();
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(nullptr)) return false;
    HANDLE h = GetClipboardData(CF_HDROP);
    bool ok = false;
    if (h) {
        auto drop = static_cast<HDROP>(h);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT cch = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(cch, L'\0');
            if (DragQueryFileW(drop, i, p.data(), cch + 1)) {
                p.resize(wcslen(p.c_str()));
                if (!p.empty()) paths->push_back(std::move(p));
            }
        }
        ok = !paths->empty();
    }
    CloseClipboard();
    return ok;
}

bool SetHdrop(HWND hwnd, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    size_t chars = 1;
    for (const auto& p : paths) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
    if (!df) {
        GlobalFree(mem);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        memcpy(dest, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
        dest += p.size() + 1;
    }
    *dest = 0;
    GlobalUnlock(mem);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

struct BridgeState {
    HWND hwnd = nullptr;
    bool to_child = false;  // host writes to-child, reads to-parent
    bool quit_on_destroy = false;
    DWORD parent_pid = 0;
    HANDLE parent = nullptr;
    uint64_t last_written_seq = 0;
    uint64_t last_applied_seq = 0;
    std::wstring last_payload;
    bool ignore_clipboard = false;
};

void PublishFromClipboard(BridgeState* st) {
    if (st->ignore_clipboard) {
        st->ignore_clipboard = false;
        return;
    }
    std::vector<std::wstring> paths;
    if (!ReadHdrop(&paths)) return;
    std::wstring payload = JoinPaths(paths);
    if (payload == st->last_payload) return;
    uint64_t seq = GetTickCount64();
    const std::wstring file = st->to_child ? ToChildPath() : ToParentPath();
    if (!WriteList(file, seq, paths)) {
        LogWarn(L"clip write failed " + file);
        return;
    }
    st->last_written_seq = seq;
    st->last_payload = payload;
    LogInfo(L"clip published " + std::to_wstring(paths.size()) + L" file(s) seq=" + std::to_wstring(seq) +
            (st->to_child ? L" -> child" : L" -> parent"));
}

void ApplyIncoming(BridgeState* st) {
    const std::wstring file = st->to_child ? ToParentPath() : ToChildPath();
    std::string raw;
    if (!ReadUtf8File(file, raw) || raw.empty()) return;
    uint64_t seq = 0;
    std::vector<std::wstring> paths;
    if (!ParseList(raw, &seq, &paths) || paths.empty()) return;
    if (seq == st->last_applied_seq || seq == st->last_written_seq) return;
    std::wstring payload = JoinPaths(paths);
    if (payload == st->last_payload) {
        st->last_applied_seq = seq;
        return;
    }
    st->ignore_clipboard = true;
    if (!SetHdrop(st->hwnd, paths)) {
        st->ignore_clipboard = false;
        LogWarn(L"clip SetHdrop failed");
        return;
    }
    st->last_applied_seq = seq;
    st->last_payload = payload;
    LogInfo(L"clip applied " + std::to_wstring(paths.size()) + L" file(s) seq=" + std::to_wstring(seq));
}

LRESULT CALLBACK ClipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<BridgeState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CLIPBOARDUPDATE:
            if (st) PublishFromClipboard(st);
            return 0;
        case WM_TIMER:
            if (!st) return 0;
            if (wp == kTimerPoll) ApplyIncoming(st);
            if (wp == kTimerParent && st->parent) {
                if (WaitForSingleObject(st->parent, 0) != WAIT_TIMEOUT) PostQuitMessage(0);
            }
            return 0;
        case WM_DESTROY:
            RemoveClipboardFormatListener(hwnd);
            if (st && st->quit_on_destroy) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateClipWindow(BridgeState* st) {
    EnsureDir(ClipDir());
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ClipWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClipWndClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError(L"clip RegisterClass failed: " + FormatWin32(GetLastError()));
        return false;
    }
    HWND hwnd = CreateWindowExW(0, kClipWndClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), st);
    if (!hwnd) {
        LogError(L"clip message window failed: " + FormatWin32(GetLastError()));
        return false;
    }
    st->hwnd = hwnd;
    AddClipboardFormatListener(hwnd);
    SetTimer(hwnd, kTimerPoll, 400, nullptr);
    if (st->parent) SetTimer(hwnd, kTimerParent, 1000, nullptr);
    return true;
}

void DestroyClipWindow(BridgeState* st) {
    if (!st || !st->hwnd) return;
    KillTimer(st->hwnd, kTimerPoll);
    if (st->parent) KillTimer(st->hwnd, kTimerParent);
    DestroyWindow(st->hwnd);
    st->hwnd = nullptr;
}

BridgeState g_host_clip;

}  // namespace

void ClipHostStart() {
    if (g_host_clip.hwnd) return;
    g_host_clip = BridgeState{};
    g_host_clip.to_child = true;
    g_host_clip.quit_on_destroy = false;
    if (CreateClipWindow(&g_host_clip)) LogInfo(L"clip host bridge started");
}

void ClipHostStop() {
    DestroyClipWindow(&g_host_clip);
    if (g_host_clip.parent) {
        CloseHandle(g_host_clip.parent);
        g_host_clip.parent = nullptr;
    }
}

std::wstring ClipBridgeCommand(DWORD expect_session) {
    std::wstring cmd = QuoteArg(GetExePath());
    cmd += L" --clip-bridge --expect-session ";
    cmd += std::to_wstring(expect_session);
    cmd += L" --parent-pid ";
    cmd += std::to_wstring(GetCurrentProcessId());
    return cmd;
}

int CmdClipBridge(DWORD expect_session, DWORD parent_pid) {
    FreeConsole();
    LogInit();
    if (expect_session != kNoSession) {
        DWORD sid = CurrentSessionId();
        if (sid != expect_session) {
            LogError(L"clip-bridge session mismatch have=" + std::to_wstring(sid) + L" want=" +
                     std::to_wstring(expect_session));
            return 1;
        }
    }
    HRESULT hr = OleInitialize(nullptr);
    LogInfo(L"clip-bridge starting session=" + std::to_wstring(CurrentSessionId()));
    BridgeState st;
    st.to_child = false;
    st.quit_on_destroy = true;
    st.parent_pid = parent_pid;
    if (parent_pid) {
        st.parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
        if (!st.parent) LogWarn(L"clip OpenProcess parent failed: " + FormatWin32(GetLastError()));
    }
    if (!CreateClipWindow(&st)) {
        if (st.parent) CloseHandle(st.parent);
        if (hr == S_OK) OleUninitialize();
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyClipWindow(&st);
    if (st.parent) CloseHandle(st.parent);
    if (hr == S_OK) OleUninitialize();
    LogInfo(L"clip-bridge exit");
    return 0;
}
