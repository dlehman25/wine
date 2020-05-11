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
#include "spellcheckprovider.h"
#include "rpcproxy.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"

extern HRESULT WINAPI MSC_DllGetClassObject(REFCLSID, REFIID, void **) DECLSPEC_HIDDEN;

WINE_DEFAULT_DEBUG_CHANNEL(msspell);

static HINSTANCE msspell_instance;

typedef struct
{
    ISpellCheckerFactory ISpellCheckerFactory_iface;
    IUserDictionariesRegistrar IUserDictionariesRegistrar_iface;
    LONG ref;
} SpellCheckerFactoryImpl;

typedef struct
{
    ISpellChecker ISpellChecker_iface;
    LONG ref;
} SpellCheckerImpl;

typedef struct dict_node
{
    WCHAR ch;
    BOOL eow;
    struct dict_node *lt, *eq, *gt;
} dict_node;

typedef struct
{
    ISpellCheckProvider ISpellCheckProvider_iface;
    IComprehensiveSpellCheckProvider IComprehensiveSpellCheckProvider_iface;
    LONG ref;
    dict_node *dict;
} SpellCheckProviderImpl;

#define EnumString_EOL ((struct list *)~0)

typedef struct
{
    IEnumString IEnumString_iface;
    LONG ref;
    struct list strings;
    struct list *next;
} EnumString;

typedef struct
{
    struct list entry;
    WCHAR str[1];
} EnumString_node;

static ISpellCheckProvider *msspell;
static ISpellCheckProvider *provider;

static inline SpellCheckerFactoryImpl *impl_from_ISpellCheckerFactory(ISpellCheckerFactory *iface)
{
    return CONTAINING_RECORD(iface, SpellCheckerFactoryImpl, ISpellCheckerFactory_iface);
}

static inline SpellCheckerImpl *impl_from_ISpellChecker(ISpellChecker *iface)
{
    return CONTAINING_RECORD(iface, SpellCheckerImpl, ISpellChecker_iface);
}

static inline SpellCheckProviderImpl *impl_from_ISpellCheckProvider(ISpellCheckProvider *iface)
{
    return CONTAINING_RECORD(iface, SpellCheckProviderImpl, ISpellCheckProvider_iface);
}

static inline SpellCheckProviderImpl *impl_from_IComprehensiveSpellCheckProvider(
                                            IComprehensiveSpellCheckProvider *iface)
{
    return CONTAINING_RECORD(iface, SpellCheckProviderImpl,
                IComprehensiveSpellCheckProvider_iface);
}

static inline EnumString *impl_from_IEnumString(IEnumString *iface)
{
    return CONTAINING_RECORD(iface, EnumString, IEnumString_iface);
}

static WCHAR *copy_string(const WCHAR *str)
{
    ULONG len;
    WCHAR *copy;

    len = wcslen(str);
    if ((copy = CoTaskMemAlloc((len + 1) * sizeof(WCHAR))))
        memcpy(copy, str, (len + 1) * sizeof(WCHAR));
    return copy;
}

static dict_node *dict_node_new(WCHAR ch)
{
    dict_node *node;

    if (!(node = heap_alloc(sizeof(*node))))
        return NULL;

    node->ch = ch;
    node->eow = FALSE;
    node->lt = node->eq = node->gt = NULL;
    return node;
}

static BOOL dict_node_insert(dict_node **root, const WCHAR *word)
{
    dict_node *cur, **next;

    if (!*root && !(*root = dict_node_new(*word)))
        return FALSE;

    cur = *root;
    next = &cur;
    for (;;)
    {
        if (*word < cur->ch)
            next = &cur->lt;
        else if (*word > cur->ch)
            next = &cur->gt;
        else
        {
            ++word;
            if (!*word)
            {
                cur->eow = TRUE;
                return TRUE;
            }
            next = &cur->eq;
        }

        if (!*next && !(*next = dict_node_new(*word)))
            return FALSE;

        cur = *next;
    }
}

static BOOL dict_search(const dict_node *root, const WCHAR *word)
{
    const dict_node *cur;

    cur = root;
    while (cur)
    {
        if (*word < cur->ch)
            cur = cur->lt;
        else if (*word > cur->ch)
            cur = cur->gt;
        else
        {
            ++word;
            if (!*word)
                return cur->eow;
            cur = cur->eq;
        }
    }

    return FALSE;
}

static void dict_free(dict_node *root)
{
    dict_node *cur;
    dict_node *nxt;
    dict_node *top; /* stack of nodes to free */

    top = NULL;
    cur = root;
    while (cur)
    {
        while (cur->lt)
        {
            nxt = cur->lt;
            cur->lt = top; /* use 'lt' member for placing on stack */
            top = cur;
            cur = nxt;
        }

        /* cur->lt is NULL */
        if (cur->eq)
        {
            cur->lt = top;
            top = cur;

            nxt = cur->eq;
            cur->eq = NULL;
            cur = nxt;
            continue;
        }
        else if (cur->gt)
        {
            cur->lt = top;
            top = cur;

            nxt = cur->gt;
            cur->gt = NULL;
            cur = nxt;
            continue;
        }

        heap_free(cur);
        if ((cur = top))
        {
            top = top->lt;
            cur->lt = NULL;
        }
    }
}

/**********************************************************************************/
/* EnumString */
/**********************************************************************************/
static HRESULT WINAPI EnumString_QueryInterface(IEnumString *iface, REFIID riid, LPVOID *ppv)
{
    EnumString *This = impl_from_IEnumString(iface);

    TRACE("IID: %s\n", debugstr_guid(riid));

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IEnumString))
    {
        *ppv = &This->IEnumString_iface;
        IEnumString_AddRef(iface);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI EnumString_AddRef(IEnumString *iface)
{
    EnumString *This = impl_from_IEnumString(iface);
    TRACE("\n");
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI EnumString_Release(IEnumString *iface)
{
    EnumString *This = impl_from_IEnumString(iface);
    EnumString_node *node;
    struct list *head;
    ULONG ref;

    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
    {
        while ((head = list_head(&This->strings)))
        {
            list_remove(head);
            node = LIST_ENTRY(head, EnumString_node, entry);
            heap_free(node);
        }
        heap_free(This);
    }
    return ref;
}

static HRESULT WINAPI EnumString_Next(IEnumString *iface, ULONG count, LPOLESTR *strings,
    ULONG *fetched)
{
    EnumString_node *node;
    EnumString *This;
    ULONG nfetched;

    This = impl_from_IEnumString(iface);
    TRACE("(%p %u %p %p)\n", This, count, strings, fetched);

    if (!strings)
        return E_POINTER;

    if (count > 1 && !fetched)
        return E_INVALIDARG;

    if (This->next == EnumString_EOL)
        This->next = list_head(&This->strings);

    nfetched = 0;
    while (count && This->next)
    {
        node = LIST_ENTRY(This->next, EnumString_node, entry);
        *strings = copy_string(node->str);
        if (!*strings)
            break;
        This->next = list_next(&This->strings, This->next);
        strings++;
        nfetched++;
        count--;
    }
    if (fetched) *fetched = nfetched;
    return count ? S_FALSE : S_OK;
}

static HRESULT WINAPI EnumString_Skip(IEnumString *iface, ULONG count)
{
    EnumString *This = impl_from_IEnumString(iface);

    TRACE("(%p %u)\n", This, count);

    if (This->next == EnumString_EOL)
        This->next = list_head(&This->strings);

    while (count && This->next)
    {
        This->next = list_next(&This->strings, This->next);
        count--;
    }

    return count ? S_FALSE : S_OK;
}

static HRESULT WINAPI EnumString_Reset(IEnumString *iface)
{
    EnumString *This = impl_from_IEnumString(iface);

    TRACE("(%p)\n", This);

    This->next = EnumString_EOL;
    return S_OK;
}

static HRESULT WINAPI EnumString_Clone(IEnumString *iface, IEnumString **ppenum)
{
    EnumString *This = impl_from_IEnumString(iface);
    FIXME("(%p %p)\n", This, ppenum);
    return E_NOTIMPL;
}

static const IEnumStringVtbl EnumStringVtbl =
{
    EnumString_QueryInterface,
    EnumString_AddRef,
    EnumString_Release,
    EnumString_Next,
    EnumString_Skip,
    EnumString_Reset,
    EnumString_Clone
};

static HRESULT EnumString_Add(IEnumString *enumstr, LPCWSTR str)
{
    EnumString *This;
    EnumString_node *node;
    size_t len;

    len = wcslen(str);
    node = heap_alloc(FIELD_OFFSET(EnumString_node, str[len+1]));
    if (!node)
        return E_OUTOFMEMORY;

    This = impl_from_IEnumString(enumstr);
    memcpy(node->str, str, len * sizeof(*str));
    node->str[len] = 0;
    list_add_tail(&This->strings, &node->entry);
    return S_OK;
}

static HRESULT EnumString_Constructor(IEnumString **enumstr)
{
    EnumString *This;

    This = heap_alloc(sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IEnumString_iface.lpVtbl = &EnumStringVtbl;
    This->ref = 1;
    list_init(&This->strings);
    This->next = EnumString_EOL;
    *enumstr = &This->IEnumString_iface;
    return S_OK;
}

/**********************************************************************************/
/* SpellCheckProvider */
/**********************************************************************************/
#define DICT_MAX_WORD_SIZE 32 /* wc --max-line-length /usr/share/dict/words => 26 */

static HRESULT load_words(const WCHAR *path, BOOL utf16le, IEnumString **enumstr)
{
    wchar_t line[DICT_MAX_WORD_SIZE];
    HRESULT hr;
    size_t len;
    FILE *file;

    *enumstr = NULL;
    hr = EnumString_Constructor(enumstr);
    if (FAILED(hr))
        return hr;

    if (!(file = _wfopen(path, utf16le ? L"rt,ccs=utf-16le" : L"rt")))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    while (fgetws(line, ARRAY_SIZE(line), file))
    {
        if (!(len = wcslen(line)))
            continue;

        line[len-1] = 0; /* \n */
        hr = EnumString_Add(*enumstr, line);
        if (FAILED(hr))
            goto error;
    }
    fclose(file);
    return S_OK;

error:
    IEnumString_Release(*enumstr);
    *enumstr = NULL;
    fclose(file);
    return hr;
}

static HRESULT WINAPI SpellCheckProvider_QueryInterface(ISpellCheckProvider *iface,
                        REFIID riid, void **ppv)
{
    SpellCheckProviderImpl *This = impl_from_ISpellCheckProvider(iface);

    TRACE("IID: %s\n", debugstr_guid(riid));

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_ISpellCheckProvider))
    {
        *ppv = &This->ISpellCheckProvider_iface;
        ISpellCheckProvider_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IComprehensiveSpellCheckProvider))
    {
        *ppv = &This->IComprehensiveSpellCheckProvider_iface;
        IComprehensiveSpellCheckProvider_AddRef(*ppv);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI SpellCheckProvider_AddRef(ISpellCheckProvider *iface)
{
    SpellCheckProviderImpl *This = impl_from_ISpellCheckProvider(iface);
    TRACE("\n");
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI SpellCheckProvider_Release(ISpellCheckProvider *iface)
{
    SpellCheckProviderImpl *This = impl_from_ISpellCheckProvider(iface);
    ULONG ref;

    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
    {
        dict_free(This->dict);
        heap_free(This);
    }
    return ref;
}

static HRESULT WINAPI SpellCheckProvider_get_LanguageTag(ISpellCheckProvider *iface,
                        LPWSTR *tag)
{
    TRACE("(%p %p)\n", iface, tag);

    if (!tag)
        return E_POINTER;

    *tag = copy_string(L"en-US");
    return *tag ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI SpellCheckProvider_Check(ISpellCheckProvider *iface, LPCWSTR text,
                        IEnumSpellingError **errors)
{
    SpellCheckProviderImpl *This = impl_from_ISpellCheckProvider(iface);

    FIXME("(%p %s %p)\n", iface, debugstr_w(text), errors);

    if (!This->dict)
    {
       *errors = NULL;
        return S_FALSE;
    }

    /* TODO: for each word */
    dict_search(This->dict, text); /* */
    return S_OK;
}

static HRESULT WINAPI SpellCheckProvider_Suggest(ISpellCheckProvider *iface, LPCWSTR word,
                        IEnumString **suggestions)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(word), suggestions);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckProvider_GetOptionValue(ISpellCheckProvider *iface,
                        LPCWSTR option, BYTE *value)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(option), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckProvider_SetOptionValue(ISpellCheckProvider *iface,
                        LPCWSTR option, BYTE value)
{
    FIXME("(%p %s %x)\n", iface, debugstr_w(option), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckProvider_get_OptionIds(ISpellCheckProvider *iface,
                        IEnumString **ids)
{
    TRACE("(%p %p)\n", iface, ids);
    return EnumString_Constructor(ids);
}

static HRESULT WINAPI SpellCheckProvider_get_Id(ISpellCheckProvider *iface, LPWSTR *id)
{
    TRACE("(%p %p)\n", iface, id);

    if (!id)
        return E_POINTER;

    *id = copy_string(L"MsSpell");
    return *id ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI SpellCheckProvider_get_LocalizedName(ISpellCheckProvider *iface,
                        LPWSTR *name)
{
    FIXME("(%p %p)\n", iface, name);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckProvider_GetOptionDescription(ISpellCheckProvider *iface,
                        LPCWSTR option, IOptionDescription **description)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(option), description);
    return E_NOTIMPL;
}

static HRESULT WINAPI SpellCheckProvider_InitializeWordlist(ISpellCheckProvider *iface,
                        WORDLIST_TYPE type, IEnumString *wordlist)
{
    SpellCheckProviderImpl *This = impl_from_ISpellCheckProvider(iface);
    DWORD i, nlines, nfetched;
    LPOLESTR *lines;
    HRESULT hr;

    TRACE("(%p %d %p)\n", iface, type, wordlist);

    IEnumString_Reset(wordlist);
    nlines = 0;
    while (IEnumString_Skip(wordlist, 1) != S_FALSE) nlines++;

    if (!(lines = heap_alloc(nlines * sizeof(*lines))))
        return E_OUTOFMEMORY;

    /* randomizing the strings makes the tree more balanced */
    IEnumString_Reset(wordlist);
    nfetched = 0;
    hr = IEnumString_Next(wordlist, nlines, lines, &nfetched);
    if (FAILED(hr))
        goto error;

    hr = S_OK;
    while (nlines)
    {
        i = rand() % nlines;
        if (!dict_node_insert(&This->dict, lines[i]))
        {
            /* on failure, we'll at least keep what we've inserted */
            hr = E_OUTOFMEMORY;
            goto error;
        }

        CoTaskMemFree(lines[i]);
        if (i != nlines-1)
        {
            lines[i] = lines[nlines-1];
            lines[nlines-1] = NULL;
        }
        nlines--;
    }
    heap_free(lines);

    return S_OK;

error:
    for (i = 0; i < nfetched; i++)
        if (lines[i]) CoTaskMemFree(lines[i]);
    heap_free(lines);
    return hr;
}

static HRESULT WINAPI ComprehensiveSpellCheckProvider_QueryInterface(
                        IComprehensiveSpellCheckProvider *iface, REFIID riid, void **ppv)
{
    SpellCheckProviderImpl *This = impl_from_IComprehensiveSpellCheckProvider(iface);

    TRACE("IID: %s\n", debugstr_guid(riid));

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_ISpellCheckProvider))
    {
        *ppv = &This->IComprehensiveSpellCheckProvider_iface;
        IComprehensiveSpellCheckProvider_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IComprehensiveSpellCheckProvider))
    {
        *ppv = &This->IComprehensiveSpellCheckProvider_iface;
        IComprehensiveSpellCheckProvider_AddRef(*ppv);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ComprehensiveSpellCheckProvider_AddRef(
                        IComprehensiveSpellCheckProvider *iface)
{
    SpellCheckProviderImpl *This = impl_from_IComprehensiveSpellCheckProvider(iface);
    TRACE("\n");
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI ComprehensiveSpellCheckProvider_Release(
                        IComprehensiveSpellCheckProvider *iface)
{
    SpellCheckProviderImpl *This = impl_from_IComprehensiveSpellCheckProvider(iface);
    ULONG ref;

    TRACE("\n");
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0)
        heap_free(This);
    return ref;
}

static HRESULT WINAPI ComprehensiveSpellCheckProvider_ComprehensiveCheck(
                            IComprehensiveSpellCheckProvider *iface, LPCWSTR text,
                            IEnumSpellingError **error)
{
    FIXME("(%p %s %p)\n", iface, debugstr_w(text), error);
    return E_NOTIMPL;
}

static const ISpellCheckProviderVtbl SpellCheckProviderVtbl =
{
    SpellCheckProvider_QueryInterface,
    SpellCheckProvider_AddRef,
    SpellCheckProvider_Release,
    SpellCheckProvider_get_LanguageTag,
    SpellCheckProvider_Check,
    SpellCheckProvider_Suggest,
    SpellCheckProvider_GetOptionValue,
    SpellCheckProvider_SetOptionValue,
    SpellCheckProvider_get_OptionIds,
    SpellCheckProvider_get_Id,
    SpellCheckProvider_get_LocalizedName,
    SpellCheckProvider_GetOptionDescription,
    SpellCheckProvider_InitializeWordlist
};

static const IComprehensiveSpellCheckProviderVtbl ComprehensiveSpellCheckProviderVtbl =
{
    ComprehensiveSpellCheckProvider_QueryInterface,
    ComprehensiveSpellCheckProvider_AddRef,
    ComprehensiveSpellCheckProvider_Release,
    ComprehensiveSpellCheckProvider_ComprehensiveCheck
};

/**********************************************************************************/
/* UserDictionariesRegistrar */
/**********************************************************************************/
static HRESULT WINAPI UserDictionariesRegistrar_QueryInterface(IUserDictionariesRegistrar *iface,
                        REFIID riid, void **ppv)
{
    FIXME("(%p %s %p)\n", iface, debugstr_guid(riid), ppv);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI UserDictionariesRegistrar_AddRef(IUserDictionariesRegistrar *iface)
{
    FIXME("(%p)\n", iface);
    return 2;
}

static ULONG WINAPI UserDictionariesRegistrar_Release(IUserDictionariesRegistrar *iface)
{
    FIXME("(%p)\n", iface);
    return 1;
}

static HRESULT WINAPI UserDictionariesRegistrar_RegisterUserDictionary(
                        IUserDictionariesRegistrar *iface,
                        LPCWSTR path, LPCWSTR lang)
{
    FIXME("(%p %s %s)\n", iface, debugstr_w(path), debugstr_w(lang));
    return E_NOTIMPL;
}

static HRESULT WINAPI UserDictionariesRegistrar_UnregisterUserDictionary(
                        IUserDictionariesRegistrar *iface,
                        LPCWSTR path, LPCWSTR lang)
{
    FIXME("(%p %s %s)\n", iface, debugstr_w(path), debugstr_w(lang));
    return E_NOTIMPL;
}

static const IUserDictionariesRegistrarVtbl UserDictionariesRegistrarVtbl =
{
    UserDictionariesRegistrar_QueryInterface,
    UserDictionariesRegistrar_AddRef,
    UserDictionariesRegistrar_Release,
    UserDictionariesRegistrar_RegisterUserDictionary,
    UserDictionariesRegistrar_UnregisterUserDictionary
};

/**********************************************************************************/
/* SpellChecker */
/**********************************************************************************/
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
    TRACE("(%p %p)\n", iface, tag);
    return ISpellCheckProvider_get_LanguageTag(provider, tag);
}

static HRESULT WINAPI SpellChecker_Check(ISpellChecker *iface, LPCWSTR text,
                        IEnumSpellingError **errors)
{
    TRACE("(%p %s %p)\n", iface, debugstr_w(text), errors);
    return ISpellCheckProvider_Check(provider, text, errors);
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
    TRACE("(%p %p)\n", iface, ids);
    return ISpellCheckProvider_get_OptionIds(provider, ids);
}

static HRESULT WINAPI SpellChecker_get_Id(ISpellChecker *iface, LPWSTR *id)
{
    TRACE("(%p %p)\n", iface, id);
    return ISpellCheckProvider_get_Id(provider, id);
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
    else if (IsEqualGUID(riid, &IID_IUserDictionariesRegistrar))
    {
        *ppvObject = &This->IUserDictionariesRegistrar_iface;
        IUserDictionariesRegistrar_AddRef(*ppvObject);
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
    LPWSTR lang;
    HRESULT hr;

    TRACE("(%p %p)\n", iface, enumstr);

    *enumstr = NULL;
    hr = ISpellCheckProvider_get_LanguageTag(provider, &lang);
    if (FAILED(hr))
        return hr;

    hr = EnumString_Constructor(enumstr);
    if (SUCCEEDED(hr))
    {
        hr = EnumString_Add(*enumstr, lang);
        if (FAILED(hr))
        {
            IEnumString_Release(*enumstr);
            *enumstr = NULL;
        }
    }
    CoTaskMemFree(lang);
    return hr;
}

static HRESULT WINAPI SpellCheckerFactory_IsSupported(ISpellCheckerFactory *iface,
                        LPCWSTR lang, BOOL *supported)
{
    HRESULT hr;
    LPWSTR provlang;

    TRACE("(%p %s %p)\n", iface, debugstr_w(lang), supported);

    if (!lang || !supported)
        return E_POINTER;

    hr = ISpellCheckProvider_get_LanguageTag(provider, &provlang);
    if (FAILED(hr))
        return hr;

    *supported = !wcsicmp(provlang, lang);
    CoTaskMemFree(provlang);
    return S_OK;
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
    This->IUserDictionariesRegistrar_iface.lpVtbl = &UserDictionariesRegistrarVtbl;
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

static const IClassFactoryVtbl SCFactoryVtbl =
{
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    SpellCheckerFactory_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory SCFactory = { &SCFactoryVtbl };

static HRESULT init_msspell(void)
{
    static const WCHAR *linux_words = L"/usr/share/dict/words";
    SpellCheckProviderImpl *prov;
    IEnumString *words;
    HRESULT hr;

    prov = heap_alloc(sizeof(*prov));
    if (!prov)
        return E_OUTOFMEMORY;

    prov->ISpellCheckProvider_iface.lpVtbl = &SpellCheckProviderVtbl;
    prov->IComprehensiveSpellCheckProvider_iface.lpVtbl = &ComprehensiveSpellCheckProviderVtbl;
    prov->ref = 1;
    prov->dict = NULL;

    hr = load_words(linux_words, FALSE, &words);
    if (SUCCEEDED(hr))
    {
        hr = SpellCheckProvider_InitializeWordlist(&prov->ISpellCheckProvider_iface,
                                                   WORDLIST_TYPE_ADD, words);
        if (FAILED(hr))
            WARN("failed to initialize default word list (0x%x)\n", hr);

        IEnumString_Release(words);
    }
    else
        WARN("failed to load linux words %s (0x%x)\n", debugstr_w(linux_words), hr);

    /* TODO: default.dic */

    hr = SpellCheckProvider_QueryInterface(&prov->ISpellCheckProvider_iface,
            &IID_ISpellCheckProvider, (void**)&msspell);
    SpellCheckProvider_Release(&prov->ISpellCheckProvider_iface);

    return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    TRACE("(%p, %u, %p)\n", instance, reason, reserved);

    switch (reason)
    {
        case DLL_WINE_PREATTACH:
            return FALSE;    /* prefer native version */
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            if (FAILED(init_msspell()))
                return FALSE;
            msspell_instance = instance;
            provider = msspell;
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
