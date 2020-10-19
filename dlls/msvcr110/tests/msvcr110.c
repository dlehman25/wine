/*
 * Copyright 2018 Daniel Lehman
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

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>
#include <float.h>
#include <limits.h>

#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include "wine/test.h"

#include <locale.h>

#define DEFINE_EXPECT(func) \
    static BOOL expect_ ## func = FALSE, called_ ## func = FALSE

#define SET_EXPECT(func) \
    expect_ ## func = TRUE

#define CHECK_EXPECT2(func) \
    do { \
        ok(expect_ ##func, "unexpected call " #func "\n"); \
        called_ ## func = TRUE; \
    }while(0)

#define CHECK_EXPECT(func) \
    do { \
        CHECK_EXPECT2(func); \
        expect_ ## func = FALSE; \
    }while(0)

#define CHECK_CALLED(func) \
    do { \
        ok(called_ ## func, "expected " #func "\n"); \
        expect_ ## func = called_ ## func = FALSE; \
    }while(0)

#undef __thiscall
#ifdef __i386__
#define __thiscall __stdcall
#else
#define __thiscall __cdecl
#endif

typedef struct {
    void *vtable;
} Context;

typedef struct {
    Context *ctx;
} _Context;

typedef struct {
    ULONG_PTR unk0[3];
    Context *ctx;
    int scheduled;
    int completed;
    ULONG_PTR unk1[6];
} _StructuredTaskCollection;

struct _UnrealizedChore;
typedef void (__cdecl *chore_func)(void*);
typedef void (__cdecl *chore_wrapper_func)(struct _UnrealizedChore*);
typedef struct _UnrealizedChore{
    ULONG_PTR unk0[1];
    chore_func func;
    _StructuredTaskCollection *coll;
    chore_wrapper_func wrapper;
    ULONG_PTR unk1[1];
} _UnrealizedChore;

typedef struct {
    ULONG_PTR unk[16];
} _CancellationTokenState;

typedef enum {
    _NotComplete,
    _Complete,
} _TaskCollectionStatus;

static char* (CDECL *p_setlocale)(int category, const char* locale);
static size_t (CDECL *p___strncnt)(const char *str, size_t count);

static unsigned int (CDECL *p_CurrentScheduler_GetNumberOfVirtualProcessors)(void);
static unsigned int (CDECL *p__CurrentScheduler__GetNumberOfVirtualProcessors)(void);
static unsigned int (CDECL *p_CurrentScheduler_Id)(void);
static unsigned int (CDECL *p__CurrentScheduler__Id)(void);

static Context* (__cdecl *p_Context_CurrentContext)(void);
static _Context* (__cdecl *p__Context__CurrentContext)(_Context*);

static void (__thiscall *p__StructuredTaskCollection_ctor_cts)(_StructuredTaskCollection*,_CancellationTokenState*);
static void (__thiscall *p__StructuredTaskCollection_Schedule)(_StructuredTaskCollection*,_UnrealizedChore*);
static _TaskCollectionStatus (__stdcall *p__StructuredTaskCollection_RunAndWait__UnrealizedChore)(_StructuredTaskCollection*,_UnrealizedChore*);

#define SETNOFAIL(x,y) x = (void*)GetProcAddress(module,y)
#define SET(x,y) do { SETNOFAIL(x,y); ok(x != NULL, "Export '%s' not found\n", y); } while(0)

static BOOL init(void)
{
    HMODULE module;

    module = LoadLibraryA("msvcr110.dll");
    if (!module)
    {
        win_skip("msvcr110.dll not installed\n");
        return FALSE;
    }

    SET(p_setlocale, "setlocale");
    SET(p___strncnt, "__strncnt");
    SET(p_CurrentScheduler_GetNumberOfVirtualProcessors, "?GetNumberOfVirtualProcessors@CurrentScheduler@Concurrency@@SAIXZ");
    SET(p__CurrentScheduler__GetNumberOfVirtualProcessors, "?_GetNumberOfVirtualProcessors@_CurrentScheduler@details@Concurrency@@SAIXZ");
    SET(p_CurrentScheduler_Id, "?Id@CurrentScheduler@Concurrency@@SAIXZ");
    SET(p__CurrentScheduler__Id, "?_Id@_CurrentScheduler@details@Concurrency@@SAIXZ");

    SET(p__Context__CurrentContext, "?_CurrentContext@_Context@details@Concurrency@@SA?AV123@XZ");

    if(sizeof(void*) == 8)
    {
        SET(p_Context_CurrentContext, "?CurrentContext@Context@Concurrency@@SAPEAV12@XZ");

        SET(p__StructuredTaskCollection_ctor_cts, "??0_StructuredTaskCollection@details@Concurrency@@QEAA@PEAV_CancellationTokenState@12@@Z");
        SET(p__StructuredTaskCollection_Schedule, "?_Schedule@_StructuredTaskCollection@details@Concurrency@@QEAAXPEAV_UnrealizedChore@23@@Z");
        SET(p__StructuredTaskCollection_RunAndWait__UnrealizedChore, "?_RunAndWait@_StructuredTaskCollection@details@Concurrency@@QEAA?AW4_TaskCollectionStatus@23@PEAV_UnrealizedChore@23@@Z");
    }
    else
    {
        SET(p_Context_CurrentContext, "?CurrentContext@Context@Concurrency@@SAPAV12@XZ");

        SET(p__StructuredTaskCollection_ctor_cts, "??0_StructuredTaskCollection@details@Concurrency@@QAE@PAV_CancellationTokenState@12@@Z");
        SET(p__StructuredTaskCollection_Schedule, "?_Schedule@_StructuredTaskCollection@details@Concurrency@@QAEXPAV_UnrealizedChore@23@@Z");
        SET(p__StructuredTaskCollection_RunAndWait__UnrealizedChore, "?_RunAndWait@_StructuredTaskCollection@details@Concurrency@@QAG?AW4_TaskCollectionStatus@23@PAV_UnrealizedChore@23@@Z"); 
    }

    return TRUE;
}

static void test_CurrentScheduler(void)
{
    unsigned int id;
    unsigned int ncpus;
    unsigned int expect;
    SYSTEM_INFO si;

    expect = ~0;
    ncpus = p_CurrentScheduler_GetNumberOfVirtualProcessors();
    ok(ncpus == expect, "expected %x, got %x\n", expect, ncpus);
    id = p_CurrentScheduler_Id();
    ok(id == expect, "expected %u, got %u\n", expect, id);

    GetSystemInfo(&si);
    expect = si.dwNumberOfProcessors;
    /* these _CurrentScheduler calls trigger scheduler creation
       if either is commented out, the following CurrentScheduler (no _) tests will still work */
    ncpus = p__CurrentScheduler__GetNumberOfVirtualProcessors();
    id = p__CurrentScheduler__Id();
    ok(ncpus == expect, "expected %u, got %u\n", expect, ncpus);
    ok(id == 0, "expected 0, got %u\n", id);

    /* these CurrentScheduler tests assume scheduler is created */
    ncpus = p_CurrentScheduler_GetNumberOfVirtualProcessors();
    ok(ncpus == expect, "expected %u, got %u\n", expect, ncpus);
    id = p_CurrentScheduler_Id();
    ok(id == 0, "expected 0, got %u\n", id);
}

static void test_setlocale(void)
{
    int i;
    char *ret;
    static const char *names[] =
    {
        "en-us",
        "en-US",
        "EN-US",
        "syr-SY",
        "uz-Latn-uz",
    };

    for(i=0; i<ARRAY_SIZE(names); i++) {
        ret = p_setlocale(LC_ALL, names[i]);
        ok(ret != NULL, "expected success, but got NULL\n");
        ok(!strcmp(ret, names[i]), "expected %s, got %s\n", names[i], ret);
    }

    ret = p_setlocale(LC_ALL, "en-us.1250");
    ok(!ret, "setlocale(en-us.1250) succeeded (%s)\n", ret);

    p_setlocale(LC_ALL, "C");
}

static void test___strncnt(void)
{
    static const struct
    {
        const char *str;
        size_t size;
        size_t ret;
    }
    strncnt_tests[] =
    {
        { NULL, 0, 0 },
        { "a", 0, 0 },
        { "a", 1, 1 },
        { "a", 10, 1 },
        { "abc", 1, 1 },
    };
    unsigned int i;
    size_t ret;

    if (0) /* crashes */
        ret = p___strncnt(NULL, 1);

    for (i = 0; i < ARRAY_SIZE(strncnt_tests); ++i)
    {
        ret = p___strncnt(strncnt_tests[i].str, strncnt_tests[i].size);
        ok(ret == strncnt_tests[i].ret, "%u: unexpected return value %u.\n", i, (int)ret);
    }
}

DEFINE_EXPECT(chore_func0);
DEFINE_EXPECT(chore_func1);
static void *chore_func_arg;
static void test_chore_func0(void *arg)
{
    CHECK_EXPECT(chore_func0);
    chore_func_arg = arg;
}

static void test_chore_func1(void *arg)
{
    CHECK_EXPECT(chore_func1);
    chore_func_arg = arg;
}

static void test__StructuredTaskCollection(void)
{
    _StructuredTaskCollection stc;
    _TaskCollectionStatus tcs;
    _UnrealizedChore uc0;
    _UnrealizedChore uc1;
    Context *ctx;

    memset(&stc, 0, sizeof(stc));
    p__StructuredTaskCollection_ctor_cts(&stc, NULL);
    todo_wine ok(stc.completed == INT_MIN, "expected %x, got %x\n", INT_MIN, stc.completed);

    memset(&stc, 0, sizeof(stc));
    memset(&uc0, 0, sizeof(uc0));
    p__StructuredTaskCollection_Schedule(&stc, &uc0);
    todo_wine ok(uc0.coll == &stc, "expected %p, got %p\n", &stc, uc0.coll);
    todo_wine ok(!!uc0.wrapper, "expected non-NULL\n");
    todo_wine ok(stc.scheduled == 1, "expected 1, got %d\n", stc.scheduled);
    ok(stc.completed == 0, "expected 0, got %d\n", stc.completed);

    ctx = p_Context_CurrentContext();
    todo_wine ok(stc.ctx == ctx, "expected %p, got %p\n", ctx, stc.ctx);

    if (!uc0.wrapper) return;

    uc0.func = test_chore_func0;
    SET_EXPECT(chore_func0);
    uc0.wrapper(&uc0);
    CHECK_CALLED(chore_func0);

    ok(chore_func_arg == &uc0, "expected %p, got %p\n", &uc0, chore_func_arg);
    todo_wine ok(stc.scheduled == 1, "expected 1, got %d\n", stc.scheduled);
    todo_wine ok(stc.completed == 1, "expected 1, got %d\n", stc.completed);

    SET_EXPECT(chore_func0);
    SET_EXPECT(chore_func1);
    uc1.func = test_chore_func1;
    tcs = p__StructuredTaskCollection_RunAndWait__UnrealizedChore(&stc, &uc1);
    CHECK_CALLED(chore_func0);
    CHECK_CALLED(chore_func1);
    ok(tcs == _Complete, "expected %d, got %d\n", _Complete, tcs);
    ok(stc.scheduled == 0, "expected 0, got %d\n", stc.scheduled);
    ok(stc.completed == 1, "expected 1, got %d\n", stc.completed);
}

static void test_CurrentContext(void)
{
    _Context _ctx, *_pctx;
    Context *ctx;

    ctx = p_Context_CurrentContext();
    ok(!!ctx, "got NULL\n");

    if (0) /* crash Windows */
        p__Context__CurrentContext(NULL);

    _pctx = p__Context__CurrentContext(&_ctx);
    ok(_ctx.ctx == ctx, "expected %p, got %p\n", ctx, _ctx.ctx);
    ok(_pctx == &_ctx, "expected %p, got %p\n", &_ctx, _pctx);
}

START_TEST(msvcr110)
{
    if (!init()) return;
    test_CurrentScheduler(); /* MUST be first (at least among Concurrency tests) */
    test_setlocale();
    test___strncnt();
    test__StructuredTaskCollection();
    test_CurrentContext();
}
