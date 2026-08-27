#pragma once

#include "common.hpp"

#include <ocidl.h>

// IMsTscAxEvents dispinterface: {336D5562-EFA8-482E-8CB3-C5C0FC7A7DB6}
inline constexpr IID IID_IMsTscAxEvents = {
    0x336D5562, 0xEFA8, 0x482E, {0x8C, 0xB3, 0xC5, 0xC0, 0xFC, 0x7A, 0x7D, 0xB6}};

// IMsRdpExtendedSettings: {302D8188-0052-4807-806A-362B628F9AC5}
struct __declspec(uuid("302D8188-0052-4807-806A-362B628F9AC5")) IMsRdpExtendedSettings
    : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE put_Property(BSTR name, VARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Property(BSTR name, VARIANT* value) = 0;
};

struct RdpCallbacks {
    void (*on_connecting)(void*) = nullptr;
    void (*on_connected)(void*) = nullptr;
    void (*on_login)(void*) = nullptr;
    void (*on_disconnected)(void*, int reason) = nullptr;
    void (*on_fatal)(void*, int code) = nullptr;
    void (*on_go_fullscreen)(void*) = nullptr;
    void (*on_leave_fullscreen)(void*) = nullptr;
    void (*on_container_minimize)(void*) = nullptr;
    void* ctx = nullptr;
};

class RdpEventSink final : public IDispatch {
public:
    explicit RdpEventSink(RdpCallbacks cb) : cb_(cb) {}

    HRESULT Advise(IUnknown* control);
    void Unadvise();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDispatch
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override;
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override;
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames,
                                            LCID lcid, DISPID* rgDispId) override;
    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
                                     DISPPARAMS* pDispParams, VARIANT* pVarResult,
                                     EXCEPINFO* pExcepInfo, UINT* puArgErr) override;

    int last_disconnect_reason() const { return last_reason_; }

private:
    long refs_ = 1;
    RdpCallbacks cb_{};
    IConnectionPoint* cp_ = nullptr;
    DWORD cookie_ = 0;
    int last_reason_ = 0;
};
