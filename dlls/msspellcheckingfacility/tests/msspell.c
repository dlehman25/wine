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

static void test_spellchecker(void)
{
    static const WCHAR *bad = L"hello worlld";
    ULONG start, len, nsuggestions;
    ISpellCheckerFactory *factory;
    IEnumSpellingError *errors;
    LPWSTR id, replace, suggestion;
    IEnumString *suggestions;
    CORRECTIVE_ACTION action;
    ISpellChecker *checker;
    ISpellingError *err;
    WCHAR word[32];
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ISpellCheckerFactory, (void**)&factory);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    if (0) /* crash on Windows */
    {
        hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", NULL);
        hr = ISpellCheckerFactory_CreateSpellChecker(NULL, L"en-US", &checker);
    }

    hr = ISpellCheckerFactory_CreateSpellChecker(factory, NULL, &checker);
    ok(hr == E_POINTER || hr == 0x800706f4 /* apartment */, "got %x\n", hr);

    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"foobar-lang", &checker);
    ok(hr == E_INVALIDARG, "got %x\n", hr);

    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"tr-TR", &checker);
    ok(hr == E_INVALIDARG, "got %x\n", hr);

    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ISpellChecker_Release(checker);

    checker = NULL;
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    if (!checker)
        goto done;

    id = NULL;
    hr = ISpellChecker_get_Id(checker, &id);
    todo_wine ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    if (!id)
        goto done;
    ok(!wcscmp(id, L"MsSpell"), "got '%s'\n", wine_dbgstr_w(id));
    CoTaskMemFree(id);

    /* Check */

    /* no errors */
    errors = NULL;
    hr = ISpellChecker_Check(checker, L"hello world", &errors);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    err = NULL;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(hr == S_FALSE, "got 0x%x\n", hr);
    ok(!err, "got %p\n", err);
    IEnumSpellingError_Release(errors);

    /* with an error */
    errors = NULL;
    hr = ISpellChecker_Check(checker, bad, &errors);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    /* first error */
    err = NULL;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!err, "got NULL\n");

    action = 42;
    hr = ISpellingError_get_CorrectiveAction(err, &action);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(action == CORRECTIVE_ACTION_GET_SUGGESTIONS, "got %d\n", action);

    len = 0;
    hr = ISpellingError_get_Length(err, &len);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(len == 6, "got %u\n", len);

    start = 0;
    hr = ISpellingError_get_StartIndex(err, &start);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(start == 6, "got %u\n", start);

    replace = NULL;
    hr = ISpellingError_get_Replacement(err, &replace);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!wcscmp(replace, L""), "got '%s'\n", wine_dbgstr_w(replace));
    CoTaskMemFree(replace);
    ISpellingError_Release(err);

    /* no more errors */
    err = (void*)0xdeadbeef;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!err, "got %p\n", err);
    IEnumSpellingError_Release(errors);

    /* Suggest */
    memcpy(word, &bad[start], len * sizeof(WCHAR));
    word[len] = 0;
    suggestions = NULL;
    hr = ISpellChecker_Suggest(checker, word, &suggestions);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!suggestions, "got NULL\n");

    nsuggestions = 0;
    suggestion = NULL;
    while (SUCCEEDED(IEnumString_Next(suggestions, 1, &suggestion, NULL)) && suggestion)
    {
        /* not sure exact suggestions and ordering are important - just that there are some*/
        ++nsuggestions;
        CoTaskMemFree(suggestion);
    }
    ok(nsuggestions, "no suggestions found\n");
    IEnumString_Release(suggestions);

    /* Ignore */
    hr = ISpellChecker_Ignore(checker, word);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    errors = NULL;
    hr = ISpellChecker_Check(checker, bad, &errors);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    err = NULL;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(hr == S_FALSE, "got 0x%x\n", hr);
    ok(!err, "got %p\n", err);
    IEnumSpellingError_Release(errors);

done:
    if (checker) ISpellChecker_Release(checker);
    ISpellCheckerFactory_Release(factory);
}

START_TEST(msspell)
{
    static const DWORD init[] = { COINIT_MULTITHREADED, COINIT_APARTMENTTHREADED };
    ISpellCheckerFactory *factory;
    HRESULT hr;
    int i;

    for (i = 0; i < ARRAY_SIZE(init); i++)
    {
        CoInitializeEx(NULL, init[i]);

        factory = NULL;
        hr = CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_ISpellCheckerFactory, (void**)&factory);
        ok(hr == S_OK || hr == REGDB_E_CLASSNOTREG /* winxp/win2k8 */, "got 0x%x\n", hr);
        if (hr != S_OK)
        {
            win_skip("SpellCheckerFactory not supported on this platform\n");
            return;
        }
        ISpellCheckerFactory_Release(factory);

        test_factory();
        test_spellchecker();
        CoUninitialize();
    }
}
