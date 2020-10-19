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

#ifdef __i386__
#include "pshpack1.h"
struct thiscall_thunk
{
    BYTE pop_eax;    /* popl  %eax (ret addr) */
    BYTE pop_edx;    /* popl  %edx (func) */
    BYTE pop_ecx;    /* popl  %ecx (this) */
    BYTE push_eax;   /* pushl %eax */
    WORD jmp_edx;    /* jmp  *%edx */
};
#include "poppack.h"

static ULONG_PTR (WINAPI *call_thiscall_func1)( void *func, void *this );

static void init_thiscall_thunk(void)
{
    struct thiscall_thunk *thunk = VirtualAlloc( NULL, sizeof(*thunk),
            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
    thunk->pop_eax  = 0x58;   /* popl  %eax */
    thunk->pop_edx  = 0x5a;   /* popl  %edx */
    thunk->pop_ecx  = 0x59;   /* popl  %ecx */
    thunk->push_eax = 0x50;   /* pushl %eax */
    thunk->jmp_edx  = 0xe2ff; /* jmp  *%edx */
    call_thiscall_func1 = (void *)thunk;
}

#define call_func1(func,_this) call_thiscall_func1(func,_this)

#else

#define init_thiscall_thunk()
#define call_func1(func,_this) func(_this)

#endif /* __i386__ */

#undef __thiscall
#ifdef __i386__
#define __thiscall __stdcall
#else
#define __thiscall __cdecl
#endif

typedef unsigned char MSVCRT_bool;

typedef void (*vtable_ptr)(void);

typedef struct {
    const vtable_ptr *vtable;
} Context;

typedef struct {
    Context *ctx;
} _Context;

typedef struct {
    LONG *signal;
} _Cancellation_beacon;

static char* (CDECL *p_setlocale)(int category, const char* locale);
static size_t (CDECL *p___strncnt)(const char *str, size_t count);

static unsigned int (CDECL *p_CurrentScheduler_GetNumberOfVirtualProcessors)(void);
static unsigned int (CDECL *p__CurrentScheduler__GetNumberOfVirtualProcessors)(void);
static unsigned int (CDECL *p_CurrentScheduler_Id)(void);
static unsigned int (CDECL *p__CurrentScheduler__Id)(void);

static Context* (__cdecl *p_Context_CurrentContext)(void);
static _Context* (__cdecl *p__Context__CurrentContext)(_Context*);

static void (__thiscall *p__Cancellation_beacon_ctor)(_Cancellation_beacon*);
static void (__thiscall *p__Cancellation_beacon_dtor)(_Cancellation_beacon*);
static MSVCRT_bool (__thiscall *p__Cancellation_beacon__Confirm_cancel)(_Cancellation_beacon*);

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

        SET(p__Cancellation_beacon_ctor,
                "??0_Cancellation_beacon@details@Concurrency@@QEAA@XZ");
        SET(p__Cancellation_beacon_dtor,
                "??1_Cancellation_beacon@details@Concurrency@@QEAA@XZ");
        SET(p__Cancellation_beacon__Confirm_cancel,
                "?_Confirm_cancel@_Cancellation_beacon@details@Concurrency@@QEAA_NXZ");
    }
    else
    {
        SET(p_Context_CurrentContext, "?CurrentContext@Context@Concurrency@@SAPAV12@XZ");

        SET(p__Cancellation_beacon_ctor,
                "??0_Cancellation_beacon@details@Concurrency@@QAE@XZ");
        SET(p__Cancellation_beacon_dtor,
                "??1_Cancellation_beacon@details@Concurrency@@QAE@XZ");
        SET(p__Cancellation_beacon__Confirm_cancel,
                "?_Confirm_cancel@_Cancellation_beacon@details@Concurrency@@QAE_NXZ");
    }

    init_thiscall_thunk();
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

static void test__Cancellation_beacon(void)
{
    _Cancellation_beacon cb;
    MSVCRT_bool confirm;

    cb.signal = NULL;
    call_func1(p__Cancellation_beacon_ctor, &cb);
    ok(!!cb.signal, "got NULL\n");
    ok(*cb.signal == 0, "got %d\n", *cb.signal);

    confirm = call_func1(p__Cancellation_beacon__Confirm_cancel, &cb);
    ok(!confirm, "got %d\n", confirm);
    ok(*cb.signal == -1, "got %d\n", *cb.signal);

    confirm = call_func1(p__Cancellation_beacon__Confirm_cancel, &cb);
    ok(!confirm, "got %d\n", confirm);
    ok(*cb.signal == -2, "got %d\n", *cb.signal);

    *cb.signal = 42;
    confirm = call_func1(p__Cancellation_beacon__Confirm_cancel, &cb);
    ok(!confirm, "got %d\n", confirm);
    ok(*cb.signal == 41, "got %d\n", *cb.signal);

    call_func1(p__Cancellation_beacon_dtor, &cb);
}

START_TEST(msvcr110)
{
    if (!init()) return;
    test_CurrentScheduler(); /* MUST be first (at least among Concurrency tests) */
    test_setlocale();
    test___strncnt();
    test_CurrentContext();
    test__Cancellation_beacon();
}
