#include "common.hpp"

#include <vector>

namespace {

HRESULT Invoke(IDispatch* disp, const wchar_t* name, WORD flags, DISPPARAMS* params, VARIANT* result) {
    if (!disp || !name) return E_POINTER;
    DISPID id = 0;
    LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
    HRESULT hr = disp->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    EXCEPINFO excep{};
    UINT arg_err = 0;
    hr = disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, params, result, &excep, &arg_err);
    if (excep.bstrDescription) SysFreeString(excep.bstrDescription);
    if (excep.bstrSource) SysFreeString(excep.bstrSource);
    if (excep.bstrHelpFile) SysFreeString(excep.bstrHelpFile);
    return hr;
}

}  // namespace

HRESULT DispPut(IDispatch* disp, const wchar_t* name, const VARIANT& value) {
    VARIANT arg = value;
    DISPID named = DISPID_PROPERTYPUT;
    DISPPARAMS params{};
    params.cArgs = 1;
    params.rgvarg = &arg;
    params.cNamedArgs = 1;
    params.rgdispidNamedArgs = &named;
    return Invoke(disp, name, DISPATCH_PROPERTYPUT, &params, nullptr);
}

HRESULT DispGet(IDispatch* disp, const wchar_t* name, VARIANT* value) {
    DISPPARAMS params{};
    VariantInit(value);
    return Invoke(disp, name, DISPATCH_PROPERTYGET, &params, value);
}

HRESULT DispCall0(IDispatch* disp, const wchar_t* name) {
    DISPPARAMS params{};
    return Invoke(disp, name, DISPATCH_METHOD, &params, nullptr);
}

HRESULT DispCall2(IDispatch* disp, const wchar_t* name, const VARIANT& a, const VARIANT& b, VARIANT* ret) {
    const VARIANT in[] = {a, b};
    return DispCallN(disp, name, in, 2, ret);
}

HRESULT DispCallN(IDispatch* disp, const wchar_t* name, const VARIANT* in_order, UINT n, VARIANT* ret) {
    if (!disp || !name) return E_POINTER;
    VARIANT stack[8];
    VARIANT* args = stack;
    std::vector<VARIANT> heap;
    if (n > 8) {
        heap.resize(n);
        args = heap.data();
    }
    for (UINT i = 0; i < n; ++i) args[i] = in_order[n - 1 - i];
    DISPPARAMS params{};
    params.cArgs = n;
    params.rgvarg = n ? args : nullptr;
    if (ret) VariantInit(ret);
    return Invoke(disp, name, DISPATCH_METHOD, &params, ret);
}

HRESULT DispGetDispatch(IDispatch* disp, const wchar_t* name, IDispatch** out) {
    *out = nullptr;
    VARIANT v;
    VariantInit(&v);
    HRESULT hr = DispGet(disp, name, &v);
    if (FAILED(hr)) return hr;
    if (v.vt == VT_DISPATCH && v.pdispVal) {
        *out = v.pdispVal;
        v.pdispVal = nullptr;
        VariantClear(&v);
        return S_OK;
    }
    hr = VariantChangeType(&v, &v, 0, VT_DISPATCH);
    if (SUCCEEDED(hr) && v.pdispVal) {
        *out = v.pdispVal;
        v.pdispVal = nullptr;
        VariantClear(&v);
        return S_OK;
    }
    VariantClear(&v);
    return E_NOINTERFACE;
}
