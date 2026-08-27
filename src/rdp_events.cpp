#include "rdp_events.hpp"

HRESULT RdpEventSink::Advise(IUnknown* control) {
    if (!control) return E_POINTER;
    IConnectionPointContainer* cpc = nullptr;
    HRESULT hr = control->QueryInterface(IID_IConnectionPointContainer, reinterpret_cast<void**>(&cpc));
    if (FAILED(hr)) return hr;
    hr = cpc->FindConnectionPoint(IID_IMsTscAxEvents, &cp_);
    cpc->Release();
    if (FAILED(hr)) return hr;
    hr = cp_->Advise(static_cast<IDispatch*>(this), &cookie_);
    if (FAILED(hr)) {
        cp_->Release();
        cp_ = nullptr;
    }
    return hr;
}

void RdpEventSink::Unadvise() {
    if (cp_) {
        if (cookie_) cp_->Unadvise(cookie_);
        cookie_ = 0;
        cp_->Release();
        cp_ = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE RdpEventSink::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IMsTscAxEvents) {
        *ppv = static_cast<IDispatch*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE RdpEventSink::AddRef() {
    return InterlockedIncrement(&refs_);
}

ULONG STDMETHODCALLTYPE RdpEventSink::Release() {
    long n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
}

HRESULT STDMETHODCALLTYPE RdpEventSink::GetTypeInfoCount(UINT* pctinfo) {
    if (!pctinfo) return E_POINTER;
    *pctinfo = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RdpEventSink::GetTypeInfo(UINT, LCID, ITypeInfo** ppTInfo) {
    if (ppTInfo) *ppTInfo = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE RdpEventSink::GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE RdpEventSink::Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS* p,
                                               VARIANT* result, EXCEPINFO*, UINT*) {
    auto arg_i4 = [&](UINT index, int def) -> int {
        if (!p || index >= p->cArgs) return def;
        VARIANT v = p->rgvarg[p->cArgs - 1 - index];
        if (v.vt == VT_I4) return v.lVal;
        if (v.vt == VT_I2) return v.iVal;
        VARIANT conv;
        VariantInit(&conv);
        if (SUCCEEDED(VariantChangeType(&conv, &v, 0, VT_I4))) return conv.lVal;
        return def;
    };

    switch (id) {
        case 1:
            if (cb_.on_connecting) cb_.on_connecting(cb_.ctx);
            break;
        case 2:
            if (cb_.on_connected) cb_.on_connected(cb_.ctx);
            break;
        case 3:
            if (cb_.on_login) cb_.on_login(cb_.ctx);
            break;
        case 4: {
            last_reason_ = arg_i4(0, 0);
            if (cb_.on_disconnected) cb_.on_disconnected(cb_.ctx, last_reason_);
            break;
        }
        case 8:
            if (cb_.on_go_fullscreen) cb_.on_go_fullscreen(cb_.ctx);
            break;
        case 9:
            if (cb_.on_leave_fullscreen) cb_.on_leave_fullscreen(cb_.ctx);
            break;
        case 10:
            if (cb_.on_fatal) cb_.on_fatal(cb_.ctx, arg_i4(0, 0));
            break;
        case 14:
            if (cb_.on_container_minimize) cb_.on_container_minimize(cb_.ctx);
            break;
        case 15:
            if (result) {
                result->vt = VT_BOOL;
                result->boolVal = VARIANT_TRUE;
            }
            break;
        default:
            break;
    }
    return S_OK;
}
