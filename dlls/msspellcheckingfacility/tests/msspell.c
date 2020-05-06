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
#include "pathcch.h"

#include "wine/test.h"
#include "wine/heap.h"

#include "initguid.h"
#include "spellcheck.h"
#include "shlobj.h"

static BOOL dict_contains_word(const WCHAR *name, const WCHAR *word)
{
    FILE *dic;
    BOOL found;
    HRESULT hr;
    WCHAR line[128], path[MAX_PATH];

    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path);
    hr = PathCchAppend(path, ARRAY_SIZE(path), L"\\Microsoft\\Spelling\\en-US");
    if (FAILED(hr))
        return FALSE;
    hr = PathCchAppend(path, ARRAY_SIZE(path), name);
    if (FAILED(hr))
        return FALSE;

    dic = _wfopen(path, L"rt,ccs=utf-16le");
    if (!dic)
        return FALSE;

    found = FALSE;
    while (fgetws(line, ARRAY_SIZE(line), dic))
    {
        line[wcslen(line)-1] = 0; /* \n */
        if (!wcscmp(line, word))
        {
            found = TRUE;
            break;
        }
    }

    fclose(dic);
    return found;
}

static DWORD count_errors(ISpellChecker *checker, LPCWSTR text)
{
    IEnumSpellingError *errors;
    ISpellingError *err;
    DWORD nerrs;
    HRESULT hr;

    errors = NULL;
    hr = ISpellChecker_Check(checker, text, &errors);
    ok(hr == S_OK, "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    nerrs = 0;
    err = NULL;
    while ((hr = IEnumSpellingError_Next(errors, &err)) != S_FALSE)
    {
        ISpellingError_Release(err);
        nerrs++;
        err = NULL;
    }

    IEnumSpellingError_Release(errors);
    return nerrs;
}

static void test_factory(DWORD model)
{
    ISpellCheckerFactory *factory;
    IMarshal *marsh, *stdmarsh;
    ISpellChecker *checker;
    IEnumString *langs;
    BOOL supported;
    LPOLESTR lang;
    HRESULT hr;
    ULONG num;

    CoInitializeEx(NULL, model);

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

    if (model == COINIT_APARTMENTTHREADED)
    {
        marsh = NULL;
        hr = ISpellChecker_QueryInterface(checker, &IID_IMarshal, (void**)&marsh);
        ok(SUCCEEDED(hr), "got 0x%x\n", hr);

        stdmarsh = NULL;
        hr = CoGetStandardMarshal(&IID_ISpellChecker, (IUnknown *)checker, MSHCTX_INPROC,
                                  NULL, MSHLFLAGS_NORMAL, &stdmarsh);
        ok(SUCCEEDED(hr), "got 0x%x\n", hr);
        ok(stdmarsh == marsh, "expected %p, got %p\n", stdmarsh, marsh);

        IMarshal_Release(stdmarsh);
        IMarshal_Release(marsh);
    }
    else
    {
        hr = ISpellChecker_QueryInterface(checker, &IID_IMarshal, (void**)&marsh);
        ok(hr == E_NOINTERFACE, "got 0x%x\n", hr);
    }

    ISpellChecker_Release(checker);

done:
    ISpellCheckerFactory_Release(factory);
    CoUninitialize();
}

static void test_spellchecker(void)
{
    static const WCHAR *bad = L"hello worlld";
    LPWSTR id, lang, replace, suggestion;
    ULONG start, len, nsuggestions;
    IEnumString *suggestions, *ids;
    ISpellCheckerFactory *factory;
    IEnumSpellingError *errors;
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
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);

    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"foobar-lang", &checker);
    ok(hr == E_INVALIDARG, "got %x\n", hr);

    /* valid locale that may or may not be installed */
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"tr-TR", &checker);
    ok(SUCCEEDED(hr) || hr == E_INVALIDARG, "got %x\n", hr);

    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ISpellChecker_Release(checker);

    checker = NULL;
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    if (!checker)
        goto done;

    if (0) /* crash on Windows */
    {
        hr = ISpellChecker_get_Id(NULL, &id);
        hr = ISpellChecker_get_Id(checker, NULL);
    }
    id = NULL;
    hr = ISpellChecker_get_Id(checker, &id);
    todo_wine ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    if (!id)
        goto done;
    ok(!wcscmp(id, L"MsSpell"), "got '%s'\n", wine_dbgstr_w(id));
    CoTaskMemFree(id);

    if (0) /* crash on Windows */
        hr = ISpellChecker_get_OptionIds(checker, NULL);

    ids = NULL;
    hr = ISpellChecker_get_OptionIds(checker, &ids);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    id = NULL;
    while (SUCCEEDED(IEnumString_Next(ids, 1, &id, NULL)) && id)
    {
        CoTaskMemFree(id);
        id = NULL;
    }
    IEnumString_Release(ids);

    if (0) /* crash on Windows */
    {
        hr = ISpellChecker_get_LanguageTag(NULL, &lang);
        hr = ISpellChecker_get_LanguageTag(checker, NULL);
    }
    lang = NULL;
    hr = ISpellChecker_get_LanguageTag(checker, &lang);
    todo_wine ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!wcscmp(lang, L"en-US"), "got '%s'\n", wine_dbgstr_w(lang));
    CoTaskMemFree(lang);

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
        suggestion = NULL;
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

static HANDLE changed;
static HRESULT WINAPI SpellCheckerChangedEventHandler_QueryInterface(
                        ISpellCheckerChangedEventHandler *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_ISpellCheckerChangedEventHandler))
    {
        *ppv = iface;
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI SpellCheckerChangedEventHandler_AddRef(
                        ISpellCheckerChangedEventHandler *iface)
{
    return 2;
}

static ULONG WINAPI SpellCheckerChangedEventHandler_Release(
                        ISpellCheckerChangedEventHandler *iface)
{
    return 1;
}

static HRESULT WINAPI SpellCheckerChangedEventHandler_Invoke(
                        ISpellCheckerChangedEventHandler *iface, ISpellChecker *checker)
{
    SetEvent(changed);
    return S_OK;
}

static const ISpellCheckerChangedEventHandlerVtbl SpellCheckerChangedEventHandlerVtbl =
{
    SpellCheckerChangedEventHandler_QueryInterface,
    SpellCheckerChangedEventHandler_AddRef,
    SpellCheckerChangedEventHandler_Release,
    SpellCheckerChangedEventHandler_Invoke
};

static ISpellCheckerChangedEventHandler SpellCheckerChangedEventHandler =
{
    &SpellCheckerChangedEventHandlerVtbl
};

static void test_SpellChecker_AddRemove(void)
{
    const WCHAR *bad_text = L"hello worllld";
    const WCHAR *bad_word = L"worllld";
    ISpellCheckerFactory *factory;
    DWORD nerrs, cookie, ret;
    ISpellChecker2 *checker2;
    ISpellChecker *checker;
    BOOL contains;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ISpellCheckerFactory, (void**)&factory);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    checker = NULL;
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    hr = ISpellChecker_QueryInterface(checker, &IID_ISpellChecker2, (void**)&checker2);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    /* spell check before adding */
    nerrs = count_errors(checker, bad_text);
    ok(nerrs == 1, "got %u\n", nerrs);

    changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!changed, "failed, gle %u\n", GetLastError());

    cookie = 0;
    hr = ISpellChecker_add_SpellCheckerChanged(checker, &SpellCheckerChangedEventHandler,
                            &cookie);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(cookie, "got 0\n");

    /* add word to %APPDATA%/Microsoft/Spelling/<lang>/default.dic
       removes from default.exc if exists */
    hr = ISpellChecker_Add(checker, bad_word);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    /* first addition can be delayed */
    ret = WaitForSingleObject(changed, 5000);
    ok(ret == WAIT_OBJECT_0, "got %u\n", ret);

    contains = dict_contains_word(L"default.dic", bad_word);
    ok(contains, "doesn't contain %ls\n", bad_word);

    /* spell check after */
    nerrs = count_errors(checker, bad_word);
    ok(nerrs == 0, "got %u\n", nerrs);

    /* remove from default.dic, add to default.exc */
    hr = ISpellChecker2_Remove(checker2, bad_word);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    /* first addition to default.exc is delayed */
    ret = WaitForSingleObject(changed, 5000);
    ok(ret == WAIT_OBJECT_0, "got %u\n", ret);

    contains = dict_contains_word(L"default.dic", bad_word);
    ok(!contains, "contains %ls\n", bad_word);
    contains = dict_contains_word(L"default.exc", bad_word);
    ok(contains, "doesn't contain %ls\n", bad_word);

    hr = ISpellChecker_remove_SpellCheckerChanged(checker, cookie);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    CloseHandle(changed);

    /* spell check after */
    nerrs = count_errors(checker, bad_text);
    ok(nerrs == 1, "got %u\n", nerrs);

    ISpellChecker2_Release(checker2);
    ISpellChecker_Release(checker);
    ISpellCheckerFactory_Release(factory);
}

static void test_suggestions(void)
{
    static const struct test { LPCWSTR word; LPCWSTR suggestions[4]; } tests[] =
    {
        { L"ssi",       { L"si",        L"s\xed",       L"as\xed"   } },
        { L"nacion",    { L"naciones",  L"noci\xf3n",   L"naci\xf3" } }
    };
    ISpellCheckerFactory *factory;
    IEnumString *suggestions;
    ISpellChecker *checker;
    int i, j, nsug, found;
    LPWSTR suggestion;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ISpellCheckerFactory, (void**)&factory);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    checker = NULL;
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"es-ES", &checker);
    todo_wine ok(SUCCEEDED(hr) || broken(hr == E_INVALIDARG) /* w1064 */, "got 0x%x\n", hr);
    if (hr == E_INVALIDARG)
    {
        win_skip("es-ES locale not supported on this platform\n");
        return;
    }

    if (!checker)
        goto done;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        const struct test *test = &tests[i];

        suggestions = NULL;
        hr = ISpellChecker_Suggest(checker, test->word, &suggestions);
        ok(SUCCEEDED(hr), "got 0x%x\n", hr);

        for (nsug = 0; test->suggestions[nsug]; nsug++) {}

        found = 0;
        suggestion = NULL;
        while (SUCCEEDED(IEnumString_Next(suggestions, 1, &suggestion, NULL)) && suggestion)
        {
            for (j = 0; j < nsug; j++)
            {
                if (!wcscmp(test->suggestions[j], suggestion))
                {
                    found++;
                    break;
                }
            }
            CoTaskMemFree(suggestion);
            suggestion = NULL;
        }
        ok(found == nsug || broken(!found) /* w8 */, "expected %d, got %d\n", nsug, found);
        IEnumString_Release(suggestions);
    }

done:
    if (checker) ISpellChecker_Release(checker);
    ISpellCheckerFactory_Release(factory);

}

static void test_UserDictionariesRegistrar(void)
{
    static const WCHAR *helllo = L"helllo\n";
    WCHAR dicpath[MAX_PATH];
    IUserDictionariesRegistrar *registrar;
    ISpellCheckerFactory *factory;
    IEnumSpellingError *errors;
    ISpellChecker *checker;
    ISpellingError *err;
    HRESULT hr;
    FILE *dic;

    factory = NULL;
    hr = CoCreateInstance(&CLSID_SpellCheckerFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ISpellCheckerFactory, (void**)&factory);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    registrar = NULL;
    hr = ISpellCheckerFactory_QueryInterface(factory, &IID_IUserDictionariesRegistrar,
            (void**)&registrar);
    todo_wine
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    if (!registrar)
        goto done;

    /* spell check before registering */
    checker = NULL;
    hr = ISpellCheckerFactory_CreateSpellChecker(factory, L"en-US", &checker);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    errors = NULL;
    hr = ISpellChecker_Check(checker, L"helllo world", &errors);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    err = NULL;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(hr == S_OK, "got 0x%x\n", hr);
    ok(!!err, "got %p\n", err);
    ISpellingError_Release(err);
    IEnumSpellingError_Release(errors);

    /* create dictionary */
    GetTempPathW(ARRAY_SIZE(dicpath), dicpath);
    hr = PathCchAppend(dicpath, ARRAY_SIZE(dicpath), L"new-words.dic");
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);

    dic = _wfopen(dicpath, L"wt,ccs=utf-16le");
    ok(!!dic, "failed to create %ls\n", dicpath);
    fwprintf(dic, L"%s\n", helllo);
    fclose(dic);

    /* register */
    hr = IUserDictionariesRegistrar_RegisterUserDictionary(registrar, NULL, NULL);
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);
    hr = IUserDictionariesRegistrar_RegisterUserDictionary(registrar, dicpath, NULL);
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);
    hr = IUserDictionariesRegistrar_RegisterUserDictionary(registrar, NULL, L"en-US");
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);

    /* adds to HKCU\Software\Microsoft\Spelling\Dictionaries\<locale> MULTI_SZ <path> */
    hr = IUserDictionariesRegistrar_RegisterUserDictionary(registrar, dicpath, L"boguslocale");
    ok(hr == S_OK, "got %x\n", hr);
    hr = IUserDictionariesRegistrar_UnregisterUserDictionary(registrar, dicpath, L"boguslocale");
    ok(hr == S_OK, "got %x\n", hr);

    hr = IUserDictionariesRegistrar_RegisterUserDictionary(registrar, dicpath, L"en-US");
    ok(hr == S_OK, "got %x\n", hr);

    /* spell check after registering */
    errors = NULL;
    hr = ISpellChecker_Check(checker, L"helllo world", &errors);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    err = NULL;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(hr == S_FALSE, "got 0x%x\n", hr);
    ok(!err, "got %p\n", err);
    IEnumSpellingError_Release(errors);

    /* unregister */
    hr = IUserDictionariesRegistrar_UnregisterUserDictionary(registrar, NULL, NULL);
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);
    hr = IUserDictionariesRegistrar_UnregisterUserDictionary(registrar, dicpath, NULL);
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);
    hr = IUserDictionariesRegistrar_UnregisterUserDictionary(registrar, NULL, L"en-US");
    ok(hr == E_POINTER || hr == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), "got %x\n", hr);

    hr = IUserDictionariesRegistrar_UnregisterUserDictionary(registrar, dicpath, L"en-us");
    ok(hr == S_OK, "got %x\n", hr);
    hr = IUserDictionariesRegistrar_UnregisterUserDictionary(registrar, dicpath, L"en-US");
    ok(hr == S_FALSE, "got %x\n", hr);

    /* spell check after unregistering */
    errors = NULL;
    hr = ISpellChecker_Check(checker, L"hello worllld", &errors);
    ok(SUCCEEDED(hr), "got 0x%x\n", hr);
    ok(!!errors, "got NULL\n");

    err = NULL;
    hr = IEnumSpellingError_Next(errors, &err);
    ok(hr == S_OK, "got 0x%x\n", hr);
    ok(!!err, "got %p\n", err);
    ISpellingError_Release(err);
    IEnumSpellingError_Release(errors);

    DeleteFileW(dicpath);

    IUserDictionariesRegistrar_Release(registrar);
done:
    ISpellCheckerFactory_Release(factory);
}

START_TEST(msspell)
{
    ISpellCheckerFactory *factory;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

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

    test_spellchecker();
    test_suggestions();
    test_UserDictionariesRegistrar();
    test_SpellChecker_AddRemove();

    CoUninitialize();

    test_factory(COINIT_MULTITHREADED);
    test_factory(COINIT_APARTMENTTHREADED);
}
