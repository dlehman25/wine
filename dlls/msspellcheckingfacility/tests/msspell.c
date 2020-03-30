/*
 * Unit test suite for MsSpellCheckingFacility
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

#include <stdarg.h>
#include <string.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"

#include "wine/test.h"
#include "wine/heap.h"

#include "initguid.h"
#include "spellcheck.h"

static void test_factory(void)
{
    ISpellCheckerFactory *factory;
    ISpellChecker *checker;
    IEnumString *langs;
    BOOL supported;
    LPOLESTR lang;
    HRESULT hr;
    ULONG num;

    factory = NULL;
    hr = CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ISpellCheckerFactory, (void**)&factory);
    ok(hr == S_OK || hr == REGDB_E_CLASSNOTREG /* winxp/win2k8 */, "got 0x%x\n", hr);
    if (hr != S_OK)
    {
        win_skip("SpellCheckerFactory not supported on this platform\n");
        return;
    }
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!factory, "got NULL\n");

    langs = NULL;
    todo_wine {
    hr = ISpellCheckerFactory_get_SupportedLanguages(factory, &langs);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!langs, "got NULL\n");
    }

    if (!langs)
        goto done;

    num = 0;
    while ((hr = IEnumString_Next(langs, 1, &lang, NULL)) == S_OK)
    {
        ++num;
        supported = -1;
        hr = ISpellCheckerFactory_IsSupported(factory, lang, &supported);
        ok(SUCCEEDED(hr), "%ls: got 0x%x\n", lang, hr);
        ok(supported == TRUE, "%ls: not supported\n", lang);
        CoTaskMemFree(lang);
    }
    IEnumString_Release(langs);

    ok(num, "no languages supported\n");

    checker = NULL;
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!checker, "got NULL\n");

    ISpellChecker_Release(checker);

done:
    ISpellCheckerFactory_Release(factory);
}

START_TEST(msspell)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    test_factory();
    CoUninitialize();
}
