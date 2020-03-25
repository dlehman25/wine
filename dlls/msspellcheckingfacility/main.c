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

WINE_DEFAULT_DEBUG_CHANNEL(msspell);

static HINSTANCE msspell_instance;

static HRESULT WINAPI SpellCheckerFactory_CreateInstance(IClassFactory *iface, IUnknown *outer,
                                                         REFIID riid, void **ppv)
{
    FIXME("(%p %s %p)\n", outer, debugstr_guid(riid), ppv);
    *ppv = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
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
