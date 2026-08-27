#include "common.hpp"

#include <netfw.h>

#include <vector>

namespace {

const wchar_t kFwRuleName[] = L"LocalRDP loopback RDP (TCP 3389)";
const wchar_t kCredRoot[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\CredentialsDelegation";
const wchar_t kCredList[] =
    L"SOFTWARE\\Policies\\Microsoft\\Windows\\CredentialsDelegation\\AllowDefaultCredentials";

void DeleteFwRule() {
    INetFwPolicy2* policy = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
    if (FAILED(hr) || !policy) return;
    INetFwRules* rules = nullptr;
    hr = policy->get_Rules(&rules);
    if (SUCCEEDED(hr) && rules) {
        BSTR name = SysAllocString(kFwRuleName);
        INetFwRule* existing = nullptr;
        if (SUCCEEDED(rules->Item(name, &existing)) && existing) {
            existing->Release();
            HRESULT rm = rules->Remove(name);
            if (SUCCEEDED(rm)) {
                LogInfo(L"Removed leftover firewall rule");
            } else {
                LogWarn(L"Could not remove leftover firewall rule: " + FormatHr(rm));
            }
        }
        SysFreeString(name);
        rules->Release();
    }
    policy->Release();
}

bool DataIsUnwanted(const wchar_t* data) {
    return _wcsicmp(data, L"TERMSRV/localhost") == 0 || _wcsicmp(data, L"TERMSRV/127.0.0.1") == 0;
}

void StripCredDelegation() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredList, 0, KEY_READ | KEY_SET_VALUE, &k) !=
        ERROR_SUCCESS) {
        return;
    }

    struct Item {
        std::wstring data;
    };
    std::vector<Item> keep;
    std::vector<std::wstring> names;
    DWORD index = 0;
    for (;;) {
        wchar_t name[64]{};
        wchar_t data[256]{};
        DWORD nlen = 64;
        DWORD dlen = sizeof(data);
        DWORD type = 0;
        LONG st = RegEnumValueW(k, index++, name, &nlen, nullptr, &type, reinterpret_cast<LPBYTE>(data),
                                &dlen);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        names.emplace_back(name);
        if (type == REG_SZ && data[0] && !DataIsUnwanted(data)) {
            keep.push_back({data});
        }
    }
    for (const auto& n : names) {
        RegDeleteValueW(k, n.c_str());
    }
    for (size_t i = 0; i < keep.size(); ++i) {
        wchar_t new_name[16];
        swprintf_s(new_name, L"%u", static_cast<unsigned>(i + 1));
        const auto& s = keep[i].data;
        RegSetValueExW(k, new_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(s.c_str()),
                       static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);

    if (keep.empty()) {
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, kCredList);
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredRoot, 0, KEY_SET_VALUE, &root) == ERROR_SUCCESS) {
            RegDeleteValueW(root, L"AllowDefaultCredentials");
            RegDeleteValueW(root, L"ConcatenateDefaults_AllowDefault");
            RegCloseKey(root);
        }
        LogInfo(L"Removed leftover TERMSRV/localhost credential-delegation policy");
    } else {
        LogInfo(L"Removed TERMSRV/localhost from credential-delegation list; other entries kept");
    }
}

void DeletePolicyValue(const wchar_t* sub, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    LONG st = RegDeleteValueW(k, name);
    RegCloseKey(k);
    if (st == ERROR_SUCCESS) {
        LogInfo(std::wstring(L"Removed leftover policy ") + sub + L"\\" + name);
    }
}

}  // namespace

void UndoLegacyMachineChanges() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DeleteFwRule();
    StripCredDelegation();
    DeletePolicyValue(L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services",
                      L"fPromptForPassword");
    if (co == S_OK) CoUninitialize();
}

int EnsureChildSessionsForStart() {
    UndoLegacyMachineChanges();

    if (ChildSessionsEnabled()) return 0;

    if (!WTSEnableChildSessions(TRUE)) {
        DWORD err = GetLastError();
        if (!IsElevated()) {
            PrintOut(L"Enabling Windows child sessions needs a one-time administrator approval.");
            return RelaunchElevated(CommandLineTail());
        }
        PrintErr(L"WTSEnableChildSessions failed: " + FormatWin32(err));
        return static_cast<int>(ExitCode::Error);
    }
    if (!ChildSessionsEnabled()) {
        PrintErr(L"Child sessions are still disabled after WTSEnableChildSessions.");
        return static_cast<int>(ExitCode::Error);
    }
    LogInfo(L"Child sessions enabled");
    PrintOut(L"Windows child sessions enabled.");
    return 0;
}
