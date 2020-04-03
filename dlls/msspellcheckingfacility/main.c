/*
 *    MsSpellCheckingFacility
 *
 * Copyright 2020 Daniel Lehman
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS
#include <stdarg.h>

#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "spellcheck.h"
#include "rpcproxy.h"
#include "wine/debug.h"
#include "wine/heap.h"

extern HRESULT WINAPI MSC_DllGetClassObject(REFCLSID, REFIID, void **) DECLSPEC_HIDDEN;

WINE_DEFAULT_DEBUG_CHANNEL(msspell);

static HINSTANCE msspell_instance;

typedef struct
{
    ISpellCheckerFactory ISpellCheckerFactory_iface;
    LONG ref;
} SpellCheckerFactoryImpl;

typedef struct
{
    ISpellChecker ISpellChecker_iface;
    LONG ref;
} SpellCheckerImpl;

static inline SpellCheckerFactoryImpl *impl_from_ISpellCheckerFactory(ISpellCheckerFactory *iface)
{
    return CONTAINING_RECORD(iface, SpellCheckerFactoryImpl, ISpellCheckerFactory_iface);
}

static inline SpellCheckerImpl *impl_from_ISpellChecker(ISpellChecker *iface)
{
    return CONTAINING_RECORD(iface, SpellCheckerImpl, ISpellChecker_iface);
}

static HRESULT WINAPI SpellChecker_QueryInterface(ISpellChecker *iface,
                        REFIID riid,
                        void **ppvObject)
{
    SpellCheckerImpl *This = impl_from_ISpellChecker(iface);

    TRACE("IID: %s\n", debugstr_guid(riid));

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_ISpellChecker))
    {
        *ppvObject = &This->ISpellChecker_iface;
        ISpellChecker_AddRef(iface);
        return S_OK;
    }

    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI SpellChecker_AddRef(ISpellChecker *iface)
{
    SpellCheckerImpl *This = impl_from_ISpellChecker(iface);
    TRACE("\n");
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI SpellChecker_Release(ISpellChecker *iface)
{
    SpellCheckerImpl *This = impl_from_ISpellChecker(iface);
    ULONG ref;

    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
        heap_free(This);
    return ref;
}

static HRESULT WINAPI SpellChecker_get_LanguageTag(ISpellChecker *iface,
                        LPWSTR *tag)
{
    FIXME("(%p %p)\n", iface, tag);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_Check(ISpellChecker *iface, LPCWSTR text,
                        IEnumSpellingError **errors)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(text), errors);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_Suggest(ISpellChecker *iface, LPCWSTR word,
                        IEnumString **suggestions)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(word), suggestions);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_Add(ISpellChecker *iface, LPCWSTR word)
{
    FIXME("(%p %s)\n", iface, debugstr_w(word));
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_Ignore(ISpellChecker *iface, LPCWSTR word)
{
    FIXME("(%p %s)\n", iface, debugstr_w(word));
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_AutoCorrect(ISpellChecker *iface, LPCWSTR from, LPCWSTR to)
{
    FIXME("(%p %s %s)\n", iface, debugstr_w(from), debugstr_w(to));
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_GetOptionValue(ISpellChecker *iface, LPCWSTR option,
                        BYTE *value)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(option), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_get_OptionIds(ISpellChecker *iface, IEnumString **ids)
{
    FIXME("(%p %p)\n", iface, ids);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_get_Id(ISpellChecker *iface, LPWSTR *id)
{
    FIXME("(%p %p)\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_get_LocalizedName(ISpellChecker *iface, LPWSTR *name)
{
    FIXME("(%p %p)\n", iface, name);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_add_SpellCheckerChanged(ISpellChecker *iface,
                        ISpellCheckerChangedEventHandler *handler, DWORD *cookie)
{
    FIXME("(%p %p %p)\n", iface, handler, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_remove_SpellCheckerChanged(ISpellChecker *iface, DWORD cookie)
{
    FIXME("(%p %x)\n", iface, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_GetOptionDescription(ISpellChecker *iface, LPCWSTR option,
                        IOptionDescription **description)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(option), description);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellChecker_ComprehensiveCheck(ISpellChecker *iface, LPCWSTR text,
                        IEnumSpellingError **error)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(text), error);
    return E_NOTIMPL;
}

static const ISpellCheckerVtbl SpellCheckerVtbl =
{
    SpellChecker_QueryInterface,
    SpellChecker_AddRef,
    SpellChecker_Release,
    SpellChecker_get_LanguageTag,
    SpellChecker_Check,
    SpellChecker_Suggest,
    SpellChecker_Add,
    SpellChecker_Ignore,
    SpellChecker_AutoCorrect,
    SpellChecker_GetOptionValue,
    SpellChecker_get_OptionIds,
    SpellChecker_get_Id,
    SpellChecker_get_LocalizedName,
    SpellChecker_add_SpellCheckerChanged,
    SpellChecker_remove_SpellCheckerChanged,
    SpellChecker_GetOptionDescription,
    SpellChecker_ComprehensiveCheck
};

static HRESULT WINAPI SpellCheckerFactory_QueryInterface(ISpellCheckerFactory *iface,
                        REFIID riid,
                        void **ppvObject)
{
    SpellCheckerFactoryImpl *This = impl_from_ISpellCheckerFactory(iface);

    TRACE("IID: %s\n", debugstr_guid(riid));

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_ISpellCheckerFactory))
    {
        *ppvObject = &This->ISpellCheckerFactory_iface;
        ISpellCheckerFactory_AddRef(iface);
        return S_OK;
    }

    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI SpellCheckerFactory_AddRef(ISpellCheckerFactory *iface)
{
    SpellCheckerFactoryImpl *This = impl_from_ISpellCheckerFactory(iface);
    TRACE("\n");
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI SpellCheckerFactory_Release(ISpellCheckerFactory *iface)
{
    SpellCheckerFactoryImpl *This = impl_from_ISpellCheckerFactory(iface);
    ULONG ref;

    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
        heap_free(This);
    return ref;
}

static HRESULT WINAPI SpellCheckerFactory_get_SupportedLanguages(ISpellCheckerFactory *iface,
                        IEnumString **enumstr)
{
    FIXME("(%p %p)\n", iface, enumstr);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckerFactory_IsSupported(ISpellCheckerFactory *iface,
                        LPCWSTR lang, BOOL *supported)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(lang), supported);
    *supported = FALSE;
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckerFactory_CreateSpellChecker(ISpellCheckerFactory *iface,
                        LPCWSTR lang, ISpellChecker **checker)
{
    SpellCheckerImpl *obj;

    TRACE("(%p %s %p)\n", iface, debugstr_w(lang), checker);

    if (!lang)
        return E_POINTER;

    if (wcsicmp(L"en-us", lang))
    {
        *checker = NULL;
        WARN("unsupported lang %s\n", debugstr_w(lang));
        return E_INVALIDARG;
    }

    obj = heap_alloc(sizeof(*obj));
    if (!obj)
        return E_OUTOFMEMORY;

    obj->ISpellChecker_iface.lpVtbl = &SpellCheckerVtbl;
    obj->ref = 1;

    *checker = &obj->ISpellChecker_iface;
    return S_OK;
}

static const ISpellCheckerFactoryVtbl SpellCheckerFactoryVtbl =
{
    SpellCheckerFactory_QueryInterface,
    SpellCheckerFactory_AddRef,
    SpellCheckerFactory_Release,
    SpellCheckerFactory_get_SupportedLanguages,
    SpellCheckerFactory_IsSupported,
    SpellCheckerFactory_CreateSpellChecker
};

static HRESULT WINAPI SpellCheckerFactory_CreateInstance(IClassFactory *iface, IUnknown *outer,
                                                         REFIID riid, void **ppv)
{
    HRESULT hr;
    SpellCheckerFactoryImpl *This;

    TRACE("(%p %s %p)\n", outer, debugstr_guid(riid), ppv);

    *ppv = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;

    This = heap_alloc(sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->ISpellCheckerFactory_iface.lpVtbl = &SpellCheckerFactoryVtbl;
    This->ref = 1;

    hr = SpellCheckerFactory_QueryInterface(&This->ISpellCheckerFactory_iface, riid, ppv);
    SpellCheckerFactory_Release(&This->ISpellCheckerFactory_iface);
    return hr;
}

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    HRESULT hr;

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;
    if (IsEqualGUID(&IID_IUnknown, riid) ||
        IsEqualGUID(&IID_IClassFactory, riid))
    {
        TRACE("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
        *ppv = iface;

        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }
    else if (IsEqualGUID(&IID_IMarshal, riid))
    {
        static IMarshal *SCFactoryMarshal;
        IMarshal *marshal;

        if (!SCFactoryMarshal)
        {
            hr = CoGetStandardMarshal(riid, (IUnknown *)iface, MSHCTX_INPROC, NULL,
                                      MSHLFLAGS_NORMAL, &marshal);
            if (FAILED(hr))
                return hr;
            if (InterlockedCompareExchangePointer((void**)&SCFactoryMarshal, marshal, NULL))
                IMarshal_Release(marshal);
        }

        *ppv = SCFactoryMarshal;
        IMarshal_AddRef(SCFactoryMarshal);
        return S_OK;
    }

    WARN("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 2;
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 1;
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    TRACE("(%p)->(%x)\n", iface, fLock);
    return S_OK;
}

static const IClassFactoryVtbl SCFactoryVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    SpellCheckerFactory_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory SCFactory = { &SCFactoryVtbl };

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    TRACE("(%p, %u, %p)\n", instance, reason, reserved);

    switch (reason)
    {
        case DLL_WINE_PREATTACH:
            return FALSE;    /* prefer native version */
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            msspell_instance = instance;
            break;
    }

    return TRUE;
}

/***********************************************************************
 *		DllGetClassObject
 */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if(IsEqualGUID(&CLSID_SpellCheckerFactory, rclsid)) {
        TRACE("(CLSID_SpellCheckerFactory %s %p)\n", debugstr_guid(riid), ppv);
        return IClassFactory_QueryInterface(&SCFactory, riid, ppv);
    }

    if (SUCCEEDED(MSC_DllGetClassObject(rclsid, riid, ppv)))
        return S_OK;

    FIXME("Unknown object %s (iface %s)\n", debugstr_guid(rclsid), debugstr_guid(riid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

/***********************************************************************
 *          DllCanUnloadNow
 */
HRESULT WINAPI DllCanUnloadNow(void)
{
    TRACE("\n");
    return S_FALSE;
}

/***********************************************************************
 *          DllRegisterServer
 */
HRESULT WINAPI DllRegisterServer(void)
{
    TRACE("()\n");
    return __wine_register_resources(msspell_instance);
}

/***********************************************************************
 *          DllUnregisterServer
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    TRACE("()\n");
    return __wine_unregister_resources(msspell_instance);
}
