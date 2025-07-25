/*
 * Synchronization tests
 *
 * Copyright 2005 Mike McCormack for CodeWeavers
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
#include <stdlib.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>
#include <setjmp.h>

#include "wine/test.h"

#undef __fastcall
#define __fastcall __stdcall

static HANDLE (WINAPI *pCreateMemoryResourceNotification)(MEMORY_RESOURCE_NOTIFICATION_TYPE);
static BOOL   (WINAPI *pQueryMemoryResourceNotification)(HANDLE, PBOOL);
static VOID   (WINAPI *pInitOnceInitialize)(PINIT_ONCE);
static BOOL   (WINAPI *pInitOnceExecuteOnce)(PINIT_ONCE,PINIT_ONCE_FN,PVOID,LPVOID*);
static BOOL   (WINAPI *pInitOnceBeginInitialize)(PINIT_ONCE,DWORD,BOOL*,LPVOID*);
static BOOL   (WINAPI *pInitOnceComplete)(PINIT_ONCE,DWORD,LPVOID);

static BOOL   (WINAPI *pInitializeCriticalSectionEx)(CRITICAL_SECTION*,DWORD,DWORD);
static VOID   (WINAPI *pInitializeConditionVariable)(PCONDITION_VARIABLE);
static BOOL   (WINAPI *pSleepConditionVariableCS)(PCONDITION_VARIABLE,PCRITICAL_SECTION,DWORD);
static BOOL   (WINAPI *pSleepConditionVariableSRW)(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
static VOID   (WINAPI *pWakeAllConditionVariable)(PCONDITION_VARIABLE);
static VOID   (WINAPI *pWakeConditionVariable)(PCONDITION_VARIABLE);

static VOID   (WINAPI *pInitializeSRWLock)(PSRWLOCK);
static VOID   (WINAPI *pAcquireSRWLockExclusive)(PSRWLOCK);
static VOID   (WINAPI *pAcquireSRWLockShared)(PSRWLOCK);
static VOID   (WINAPI *pReleaseSRWLockExclusive)(PSRWLOCK);
static VOID   (WINAPI *pReleaseSRWLockShared)(PSRWLOCK);
static BOOLEAN (WINAPI *pTryAcquireSRWLockExclusive)(PSRWLOCK);
static BOOLEAN (WINAPI *pTryAcquireSRWLockShared)(PSRWLOCK);

static NTSTATUS (WINAPI *pNtAllocateVirtualMemory)(HANDLE, PVOID *, ULONG_PTR, SIZE_T *, ULONG, ULONG);
static NTSTATUS (WINAPI *pNtFreeVirtualMemory)(HANDLE, PVOID *, SIZE_T *, ULONG);
static NTSTATUS (WINAPI *pNtWaitForSingleObject)(HANDLE, BOOLEAN, const LARGE_INTEGER *);
static NTSTATUS (WINAPI *pNtWaitForMultipleObjects)(ULONG,const HANDLE*,BOOLEAN,BOOLEAN,const LARGE_INTEGER*);
static PSLIST_ENTRY (__fastcall *pRtlInterlockedPushListSList)(PSLIST_HEADER list, PSLIST_ENTRY first,
                                                               PSLIST_ENTRY last, ULONG count);
static PSLIST_ENTRY (WINAPI *pRtlInterlockedPushListSListEx)(PSLIST_HEADER list, PSLIST_ENTRY first,
                                                             PSLIST_ENTRY last, ULONG count);
static NTSTATUS (WINAPI *pNtQueueApcThread)(HANDLE,PNTAPCFUNC,ULONG_PTR,ULONG_PTR,ULONG_PTR);
static NTSTATUS (WINAPI *pNtTestAlert)(void);

#ifdef __i386__

#pragma pack(push,1)
struct fastcall_thunk
{
    BYTE pop_edx;   /* popl %edx            (ret addr) */
    BYTE pop_eax;   /* popl %eax            (func) */
    BYTE pop_ecx;   /* popl %ecx            (param 1) */
    BYTE xchg[3];   /* xchgl (%esp),%edx    (param 2) */
    WORD jmp_eax;   /* jmp  *%eax */
};
#pragma pack(pop)

static void * (WINAPI *call_fastcall_func4)(void *func, const void *a, const void *b, const void *c, const void *d);

static void init_fastcall_thunk(void)
{
    struct fastcall_thunk *thunk = VirtualAlloc(NULL, sizeof(*thunk), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    thunk->pop_edx = 0x5a;      /* popl  %edx */
    thunk->pop_eax = 0x58;      /* popl  %eax */
    thunk->pop_ecx = 0x59;      /* popl  %ecx */
    thunk->xchg[0] = 0x87;      /* xchgl (%esp),%edx */
    thunk->xchg[1] = 0x14;
    thunk->xchg[2] = 0x24;
    thunk->jmp_eax = 0xe0ff;    /* jmp *%eax */
    call_fastcall_func4 = (void *)thunk;
}

#define call_func4(func, a, b, c, d) call_fastcall_func4(func, (const void *)(a), \
        (const void *)(b), (const void *)(c), (const void *)(d))

#else  /* __i386__ */

#define init_fastcall_thunk() do { } while(0)
#define call_func4(func, a, b, c, d) func(a, b, c, d)

#endif /* __i386__ */

static void test_signalandwait(void)
{
    DWORD r;
    HANDLE event[2], semaphore[2], file;
    int i;

    /* invalid parameters */
    r = SignalObjectAndWait(NULL, NULL, 0, 0);
    ok( r == WAIT_FAILED, "should fail\n");

    event[0] = CreateEventW(NULL, 0, 0, NULL);
    event[1] = CreateEventW(NULL, 1, 1, NULL);

    ok( event[0] && event[1], "failed to create event flags\n");

    r = SignalObjectAndWait(event[0], NULL, 0, FALSE);
    ok( r == WAIT_FAILED, "should fail\n");

    r = SignalObjectAndWait(NULL, event[0], 0, FALSE);
    ok( r == WAIT_FAILED, "should fail\n");


    /* valid parameters */
    r = SignalObjectAndWait(event[0], event[1], 0, FALSE);
    ok( r == WAIT_OBJECT_0, "should succeed\n");

    /* event[0] is now signalled - we repeat this test multiple times
     * to ensure that the wineserver handles this situation properly. */
    for (i = 0; i < 10000; i++)
    {
        r = SignalObjectAndWait(event[0], event[0], 0, FALSE);
        ok(r == WAIT_OBJECT_0, "should succeed\n");
    }

    /* event[0] is not signalled */
    r = WaitForSingleObject(event[0], 0);
    ok( r == WAIT_TIMEOUT, "event was signalled\n");

    r = SignalObjectAndWait(event[0], event[0], 0, FALSE);
    ok( r == WAIT_OBJECT_0, "should succeed\n");

    /* clear event[1] and check for a timeout */
    ok(ResetEvent(event[1]), "failed to clear event[1]\n");
    r = SignalObjectAndWait(event[0], event[1], 0, FALSE);
    ok( r == WAIT_TIMEOUT, "should timeout\n");

    CloseHandle(event[0]);
    CloseHandle(event[1]);

    /* semaphores */
    semaphore[0] = CreateSemaphoreW( NULL, 0, 1, NULL );
    semaphore[1] = CreateSemaphoreW( NULL, 1, 1, NULL );
    ok( semaphore[0] && semaphore[1], "failed to create semaphore\n");

    r = SignalObjectAndWait(semaphore[0], semaphore[1], 0, FALSE);
    ok( r == WAIT_OBJECT_0, "should succeed\n");

    r = SignalObjectAndWait(semaphore[0], semaphore[1], 0, FALSE);
    ok( r == WAIT_FAILED, "should fail\n");

    r = ReleaseSemaphore(semaphore[0],1,NULL);
    ok( r == FALSE, "should fail\n");

    r = ReleaseSemaphore(semaphore[1],1,NULL);
    ok( r == TRUE, "should succeed\n");

    CloseHandle(semaphore[0]);
    CloseHandle(semaphore[1]);

    /* try a registry key */
    file = CreateFileA("x", GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    r = SignalObjectAndWait(file, file, 0, FALSE);
    ok( r == WAIT_FAILED, "should fail\n");
    ok( ERROR_INVALID_HANDLE == GetLastError(), "should return invalid handle error\n");
    CloseHandle(file);
}

static void test_temporary_objects(void)
{
    HANDLE handle;

    SetLastError(0xdeadbeef);
    handle = CreateMutexA(NULL, FALSE, "WineTestMutex2");
    ok(handle != NULL, "CreateMutex failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    SetLastError(0xdeadbeef);
    handle = OpenMutexA(READ_CONTROL, FALSE, "WineTestMutex2");
    ok(!handle, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle = CreateSemaphoreA(NULL, 0, 1, "WineTestSemaphore2");
    ok(handle != NULL, "CreateSemaphore failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    SetLastError(0xdeadbeef);
    handle = OpenSemaphoreA(READ_CONTROL, FALSE, "WineTestSemaphore2");
    ok(!handle, "OpenSemaphore succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle = CreateEventA(NULL, FALSE, FALSE, "WineTestEvent2");
    ok(handle != NULL, "CreateEvent failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    SetLastError(0xdeadbeef);
    handle = OpenEventA(READ_CONTROL, FALSE, "WineTestEvent2");
    ok(!handle, "OpenEvent succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle = CreateWaitableTimerA(NULL, FALSE, "WineTestWaitableTimer2");
    ok(handle != NULL, "CreateWaitableTimer failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    SetLastError(0xdeadbeef);
    handle = OpenWaitableTimerA(READ_CONTROL, FALSE, "WineTestWaitableTimer2");
    ok(!handle, "OpenWaitableTimer succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());
}

struct test_mutex_thread_params
{
    HANDLE mutex;
    HANDLE start_event;
    HANDLE stop_event;
    BOOL owner;
};

static DWORD WINAPI test_mutex_thread(void *arg)
{
    struct test_mutex_thread_params *params = arg;
    DWORD ret;

    ret = WaitForSingleObject(params->mutex, INFINITE);
    if (params->owner) ok(!ret, "got %#lx\n", ret);
    else ok(ret == WAIT_ABANDONED, "got %#lx\n", ret);
    SetEvent(params->start_event);

    ret = WaitForSingleObject(params->stop_event, INFINITE);
    ok(!ret, "got %#lx\n", ret);
    return 0;
}

static void test_mutex(void)
{
    DWORD wait_ret;
    BOOL ret;
    HANDLE hCreated, hOpened, owner_thread, waiter_thread;
    struct test_mutex_thread_params params;
    int i;
    DWORD failed = 0;

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(0, FALSE, "WineTestMutex");
    ok(hOpened == NULL, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hCreated = CreateMutexA(NULL, FALSE, "WineTestMutex");
    ok(hCreated != NULL, "CreateMutex failed with error %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(0, FALSE, "WineTestMutex");
    ok(hOpened == NULL, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_ACCESS_DENIED, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(GENERIC_EXECUTE, FALSE, "WineTestMutex");
    ok(hOpened != NULL, "OpenMutex failed with error %ld\n", GetLastError());
    wait_ret = WaitForSingleObject(hOpened, INFINITE);
    ok(wait_ret == WAIT_OBJECT_0, "WaitForSingleObject failed with error %ld\n", GetLastError());
    CloseHandle(hOpened);

    for(i=0; i < 31; i++)
    {
        wait_ret = WaitForSingleObject(hCreated, INFINITE);
        ok(wait_ret == WAIT_OBJECT_0, "WaitForSingleObject failed with error 0x%08lx\n", wait_ret);
    }

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(GENERIC_READ | GENERIC_WRITE, FALSE, "WineTestMutex");
    ok(hOpened != NULL, "OpenMutex failed with error %ld\n", GetLastError());
    wait_ret = WaitForSingleObject(hOpened, INFINITE);
    ok(wait_ret == WAIT_FAILED, "WaitForSingleObject succeeded\n");
    CloseHandle(hOpened);

    for (i = 0; i < 32; i++)
    {
        SetLastError(0xdeadbeef);
        hOpened = OpenMutexA(0x1 << i, FALSE, "WineTestMutex");
        if(hOpened != NULL)
        {
            SetLastError(0xdeadbeef);
            ret = ReleaseMutex(hOpened);
            ok(ret, "ReleaseMutex failed with error %ld, access %x\n", GetLastError(), 1 << i);
            CloseHandle(hOpened);
        }
        else
        {
            if ((1 << i) == ACCESS_SYSTEM_SECURITY)
                todo_wine ok(GetLastError() == ERROR_PRIVILEGE_NOT_HELD, "wrong error %lu, access %x\n", GetLastError(), 1 << i);
            else
                ok(GetLastError() == ERROR_ACCESS_DENIED, "wrong error %lu, , access %x\n", GetLastError(), 1 << i);
            ReleaseMutex(hCreated);
            failed |=0x1 << i;
        }
    }

    todo_wine
    ok( failed == 0x0de0fffe, "open succeeded when it shouldn't: %lx\n", failed);

    SetLastError(0xdeadbeef);
    ret = ReleaseMutex(hCreated);
    ok(!ret && (GetLastError() == ERROR_NOT_OWNER),
        "ReleaseMutex should have failed with ERROR_NOT_OWNER instead of %ld\n", GetLastError());

    /* test case sensitivity */

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(READ_CONTROL, FALSE, "WINETESTMUTEX");
    ok(!hOpened, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(READ_CONTROL, FALSE, "winetestmutex");
    ok(!hOpened, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexA(READ_CONTROL, FALSE, NULL);
    ok(!hOpened, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hOpened = OpenMutexW(READ_CONTROL, FALSE, NULL);
    ok(!hOpened, "OpenMutex succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hOpened = CreateMutexA(NULL, FALSE, "WineTestMutex");
    ok(hOpened != NULL, "CreateMutex failed with error %ld\n", GetLastError());
    ok(GetLastError() == ERROR_ALREADY_EXISTS, "wrong error %lu\n", GetLastError());
    CloseHandle(hOpened);

    SetLastError(0xdeadbeef);
    hOpened = CreateMutexA(NULL, FALSE, "WINETESTMUTEX");
    ok(hOpened != NULL, "CreateMutex failed with error %ld\n", GetLastError());
    ok(GetLastError() == 0, "wrong error %lu\n", GetLastError());
    CloseHandle(hOpened);

    CloseHandle(hCreated);

    params.start_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    params.stop_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    params.mutex = CreateMutexA(NULL, FALSE, NULL);

    params.owner = TRUE;
    owner_thread = CreateThread(NULL, 0, test_mutex_thread, &params, 0, NULL);
    ok(!!owner_thread, "CreateThread failed, error %lu\n", GetLastError());
    ret = WaitForSingleObject(params.start_event, 1000);
    ok(!ret, "got %#x\n", ret);

    params.owner = FALSE;
    waiter_thread = CreateThread(NULL, 0, test_mutex_thread, &params, 0, NULL);
    ok(!!waiter_thread, "CreateThread failed, error %lu\n", GetLastError());
    ret = WaitForSingleObject(params.start_event, 100);
    ok(ret == WAIT_TIMEOUT, "got %#x\n", ret);

    CloseHandle(params.mutex);
    ret = WaitForSingleObject(params.start_event, 100);
    ok(ret == WAIT_TIMEOUT, "got %#x\n", ret);

    TerminateThread(owner_thread, 0);
    ret = WaitForSingleObject(owner_thread, 1000);
    ok(!ret, "got %#x\n", ret);
    ret = WaitForSingleObject(params.start_event, 1000);
    ok(!ret, "got %#x\n", ret);

    SetEvent(params.stop_event);
    ret = WaitForSingleObject(waiter_thread, 1000);
    ok(!ret, "got %#x\n", ret);

    CloseHandle(owner_thread);
    CloseHandle(waiter_thread);

    CloseHandle(params.start_event);
    CloseHandle(params.stop_event);
}

static void test_slist(void)
{
    struct item
    {
        SLIST_ENTRY entry;
        int value;
    } item1, item2, item3, *item;
    SLIST_HEADER slist_header;
    SLIST_ENTRY *entry;
    USHORT size;
    int i;

    item1.value = 1;
    item2.value = 2;
    item3.value = 3;

    memset(&slist_header, 0xff, sizeof(slist_header));
    InitializeSListHead(&slist_header);
    size = QueryDepthSList(&slist_header);
    ok(size == 0, "Expected size == 0, got %u\n", size);

    /* test PushEntry, PopEntry and Flush */
    entry = InterlockedPushEntrySList(&slist_header, &item1.entry);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
    size = QueryDepthSList(&slist_header);
    ok(size == 1, "Expected size == 1, got %u\n", size);

    entry = InterlockedPushEntrySList(&slist_header, &item2.entry);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 1, "Expected item->value == 1, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 2, "Expected size == 2, got %u\n", size);

    entry = InterlockedPushEntrySList(&slist_header, &item3.entry);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 2, "Expected item->value == 2, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 3, "Expected size == 3, got %u\n", size);

    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 3, "Expected item->value == 3, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 2, "Expected size == 2, got %u\n", size);

    entry = InterlockedFlushSList(&slist_header);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 2, "Expected item->value == 2, got %u\n", item->value);
    item = CONTAINING_RECORD(item->entry.Next, struct item, entry);
    ok(item->value == 1, "Expected item->value == 1, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 0, "Expected size == 0, got %u\n", size);
    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);

    /* test RtlInterlockedPushListSList */
    entry = InterlockedPushEntrySList(&slist_header, &item3.entry);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
    entry = call_func4(pRtlInterlockedPushListSList, &slist_header, &item2.entry, &item1.entry, 42);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 3, "Expected item->value == 3, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 43, "Expected size == 43, got %u\n", size);

    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 2, "Expected item->value == 2, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 42, "Expected size == 42, got %u\n", size);

    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 1, "Expected item->value == 1, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 41, "Expected size == 41, got %u\n", size);

    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 3, "Expected item->value == 3, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 40, "Expected size == 40, got %u\n", size);

    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
    size = QueryDepthSList(&slist_header);
    ok(size == 40, "Expected size == 40, got %u\n", size);

    entry = InterlockedFlushSList(&slist_header);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
    size = QueryDepthSList(&slist_header);
    ok(size == 40 || broken(size == 0) /* >= Win 8 */, "Expected size == 40, got %u\n", size);

    entry = InterlockedPushEntrySList(&slist_header, &item1.entry);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
    entry = InterlockedFlushSList(&slist_header);
    ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
    item = CONTAINING_RECORD(entry, struct item, entry);
    ok(item->value == 1, "Expected item->value == 1, got %u\n", item->value);
    size = QueryDepthSList(&slist_header);
    ok(size == 0, "Expected size == 0, got %u\n", size);

    /* test RtlInterlockedPushListSListEx */
    if (pRtlInterlockedPushListSListEx)
    {
        entry = InterlockedPushEntrySList(&slist_header, &item3.entry);
        ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
        entry = pRtlInterlockedPushListSListEx(&slist_header, &item2.entry, &item1.entry, 42);
        ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
        item = CONTAINING_RECORD(entry, struct item, entry);
        ok(item->value == 3, "Expected item->value == 3, got %u\n", item->value);
        size = QueryDepthSList(&slist_header);
        ok(size == 43, "Expected size == 43, got %u\n", size);

        entry = InterlockedFlushSList(&slist_header);
        ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
        item = CONTAINING_RECORD(entry, struct item, entry);
        ok(item->value == 2, "Expected item->value == 2, got %u\n", item->value);
        item = CONTAINING_RECORD(item->entry.Next, struct item, entry);
        ok(item->value == 1, "Expected item->value == 1, got %u\n", item->value);
        item = CONTAINING_RECORD(item->entry.Next, struct item, entry);
        ok(item->value == 3, "Expected item->value == 3, got %u\n", item->value);
        size = QueryDepthSList(&slist_header);
        ok(size == 0, "Expected size == 0, got %u\n", size);
    }
    else
        win_skip("RtlInterlockedPushListSListEx not available, skipping tests\n");

    /* test with a lot of items */
    for (i = 0; i < 65536; i++)
    {
        item = HeapAlloc(GetProcessHeap(), 0, sizeof(*item));
        item->value = i + 1;
        entry = InterlockedPushEntrySList(&slist_header, &item->entry);
        if (i)
        {
            ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
            item = CONTAINING_RECORD(entry, struct item, entry);
            ok(item->value == i, "Expected item->value == %u, got %u\n", i, item->value);
        }
        else
        {
            ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
        }
        size = QueryDepthSList(&slist_header);
        ok(size == ((i + 1) & 0xffff), "Expected size == %u, got %u\n", (i + 1) & 0xffff, size);
    }

    entry = InterlockedFlushSList(&slist_header);
    for (i = 65536; i > 0; i--)
    {
        ok(entry != NULL, "Expected entry != NULL, got %p\n", entry);
        item = CONTAINING_RECORD(entry, struct item, entry);
        ok(item->value == i, "Expected item->value == %u, got %u\n", i, item->value);
        entry = item->entry.Next;
        HeapFree(GetProcessHeap(), 0, item);
    }
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
    size = QueryDepthSList(&slist_header);
    ok(size == 0, "Expected size == 0, got %u\n", size);
    entry = InterlockedPopEntrySList(&slist_header);
    ok(entry == NULL, "Expected entry == NULL, got %p\n", entry);
}

static void test_event(void)
{
    HANDLE handle, handle2;
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    ACL acl;
    DWORD ret;
    BOOL val;

    /* no sd */
    handle = CreateEventA(NULL, FALSE, FALSE, __FILE__ ": Test Event");
    ok(handle != NULL, "CreateEventW with blank sd failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);

    /* blank sd */
    handle = CreateEventA(&sa, FALSE, FALSE, __FILE__ ": Test Event");
    ok(handle != NULL, "CreateEventW with blank sd failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    /* sd with NULL dacl */
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    handle = CreateEventA(&sa, FALSE, FALSE, __FILE__ ": Test Event");
    ok(handle != NULL, "CreateEventW with blank sd failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    /* sd with empty dacl */
    InitializeAcl(&acl, sizeof(acl), ACL_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, &acl, FALSE);
    handle = CreateEventA(&sa, FALSE, FALSE, __FILE__ ": Test Event");
    ok(handle != NULL, "CreateEventW with blank sd failed with error %ld\n", GetLastError());
    CloseHandle(handle);

    /* test case sensitivity */

    SetLastError(0xdeadbeef);
    handle = CreateEventA(NULL, FALSE, FALSE, __FILE__ ": Test Event");
    ok( handle != NULL, "CreateEvent failed with error %lu\n", GetLastError());
    ok( GetLastError() == 0, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = CreateEventA(NULL, FALSE, FALSE, __FILE__ ": Test Event");
    ok( handle2 != NULL, "CreateEvent failed with error %ld\n", GetLastError());
    ok( GetLastError() == ERROR_ALREADY_EXISTS, "wrong error %lu\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = CreateEventA(NULL, FALSE, FALSE, __FILE__ ": TEST EVENT");
    ok( handle2 != NULL, "CreateEvent failed with error %ld\n", GetLastError());
    ok( GetLastError() == 0, "wrong error %lu\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = OpenEventA( EVENT_ALL_ACCESS, FALSE, __FILE__ ": Test Event");
    ok( handle2 != NULL, "OpenEvent failed with error %ld\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = OpenEventA( EVENT_ALL_ACCESS, FALSE, __FILE__ ": TEST EVENT");
    ok( !handle2, "OpenEvent succeeded\n");
    ok( GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = OpenEventA( EVENT_ALL_ACCESS, FALSE, NULL );
    ok( !handle2, "OpenEvent succeeded\n");
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = OpenEventW( EVENT_ALL_ACCESS, FALSE, NULL );
    ok( !handle2, "OpenEvent succeeded\n");
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    CloseHandle( handle );

    /* resource notifications are events too */

    if (!pCreateMemoryResourceNotification || !pQueryMemoryResourceNotification)
    {
        trace( "memory resource notifications not supported\n" );
        return;
    }
    handle = pCreateMemoryResourceNotification( HighMemoryResourceNotification + 1 );
    ok( !handle, "CreateMemoryResourceNotification succeeded\n" );
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError() );
    ret = pQueryMemoryResourceNotification( handle, &val );
    ok( !ret, "QueryMemoryResourceNotification succeeded\n" );
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError() );

    handle = pCreateMemoryResourceNotification( LowMemoryResourceNotification );
    ok( handle != 0, "CreateMemoryResourceNotification failed err %lu\n", GetLastError() );
    ret = WaitForSingleObject( handle, 10 );
    ok( ret == WAIT_OBJECT_0 || ret == WAIT_TIMEOUT, "WaitForSingleObject wrong ret %lu\n", ret );

    val = ~0;
    ret = pQueryMemoryResourceNotification( handle, &val );
    ok( ret, "QueryMemoryResourceNotification failed err %lu\n", GetLastError() );
    ok( val == FALSE || val == TRUE, "wrong value %u\n", val );
    ret = CloseHandle( handle );
    ok( ret, "CloseHandle failed err %lu\n", GetLastError() );

    handle = CreateEventA(NULL, FALSE, FALSE, __FILE__ ": Test Event");
    val = ~0;
    ret = pQueryMemoryResourceNotification( handle, &val );
    ok( ret, "QueryMemoryResourceNotification failed err %lu\n", GetLastError() );
    ok( val == FALSE || val == TRUE, "wrong value %u\n", val );
    CloseHandle( handle );
}

static void test_semaphore(void)
{
    HANDLE handle, handle2;

    /* test case sensitivity */

    SetLastError(0xdeadbeef);
    handle = CreateSemaphoreA(NULL, 0, 1, __FILE__ ": Test Semaphore");
    ok(handle != NULL, "CreateSemaphore failed with error %lu\n", GetLastError());
    ok(GetLastError() == 0, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = CreateSemaphoreA(NULL, 0, 1, __FILE__ ": Test Semaphore");
    ok( handle2 != NULL, "CreateSemaphore failed with error %ld\n", GetLastError());
    ok( GetLastError() == ERROR_ALREADY_EXISTS, "wrong error %lu\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = CreateSemaphoreA(NULL, 0, 1, __FILE__ ": TEST SEMAPHORE");
    ok( handle2 != NULL, "CreateSemaphore failed with error %ld\n", GetLastError());
    ok( GetLastError() == 0, "wrong error %lu\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = OpenSemaphoreA( SEMAPHORE_ALL_ACCESS, FALSE, __FILE__ ": Test Semaphore");
    ok( handle2 != NULL, "OpenSemaphore failed with error %ld\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = OpenSemaphoreA( SEMAPHORE_ALL_ACCESS, FALSE, __FILE__ ": TEST SEMAPHORE");
    ok( !handle2, "OpenSemaphore succeeded\n");
    ok( GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = OpenSemaphoreA( SEMAPHORE_ALL_ACCESS, FALSE, NULL );
    ok( !handle2, "OpenSemaphore succeeded\n");
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = OpenSemaphoreW( SEMAPHORE_ALL_ACCESS, FALSE, NULL );
    ok( !handle2, "OpenSemaphore succeeded\n");
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    CloseHandle( handle );
}

static void test_waitable_timer(void)
{
    HANDLE handle, handle2;

    /* test case sensitivity */

    SetLastError(0xdeadbeef);
    handle = CreateWaitableTimerA(NULL, FALSE, __FILE__ ": Test WaitableTimer");
    ok(handle != NULL, "CreateWaitableTimer failed with error %lu\n", GetLastError());
    ok(GetLastError() == 0, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = CreateWaitableTimerA(NULL, FALSE, __FILE__ ": Test WaitableTimer");
    ok( handle2 != NULL, "CreateWaitableTimer failed with error %ld\n", GetLastError());
    ok( GetLastError() == ERROR_ALREADY_EXISTS, "wrong error %lu\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = CreateWaitableTimerA(NULL, FALSE, __FILE__ ": TEST WAITABLETIMER");
    ok( handle2 != NULL, "CreateWaitableTimer failed with error %ld\n", GetLastError());
    ok( GetLastError() == 0, "wrong error %lu\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = OpenWaitableTimerA( TIMER_ALL_ACCESS, FALSE, __FILE__ ": Test WaitableTimer");
    ok( handle2 != NULL, "OpenWaitableTimer failed with error %ld\n", GetLastError());
    CloseHandle( handle2 );

    SetLastError(0xdeadbeef);
    handle2 = OpenWaitableTimerA( TIMER_ALL_ACCESS, FALSE, __FILE__ ": TEST WAITABLETIMER");
    ok( !handle2, "OpenWaitableTimer succeeded\n");
    ok( GetLastError() == ERROR_FILE_NOT_FOUND, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = OpenWaitableTimerA( TIMER_ALL_ACCESS, FALSE, NULL );
    ok( !handle2, "OpenWaitableTimer failed with error %ld\n", GetLastError());
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    handle2 = OpenWaitableTimerW( TIMER_ALL_ACCESS, FALSE, NULL );
    ok( !handle2, "OpenWaitableTimer failed with error %ld\n", GetLastError());
    ok( GetLastError() == ERROR_INVALID_PARAMETER, "wrong error %lu\n", GetLastError());

    CloseHandle( handle );
}

static HANDLE sem = 0;

static void CALLBACK iocp_callback(DWORD dwErrorCode, DWORD dwNumberOfBytesTransferred, LPOVERLAPPED lpOverlapped)
{
    ReleaseSemaphore(sem, 1, NULL);
}

static BOOL (WINAPI *p_BindIoCompletionCallback)( HANDLE FileHandle, LPOVERLAPPED_COMPLETION_ROUTINE Function, ULONG Flags) = NULL;

static void test_iocp_callback(void)
{
    char temp_path[MAX_PATH];
    char filename[MAX_PATH];
    DWORD ret;
    BOOL retb;
    static const char prefix[] = "pfx";
    HANDLE hFile;
    HMODULE hmod = GetModuleHandleA("kernel32.dll");
    DWORD bytesWritten;
    const char *buffer = "12345678123456781234567812345678";
    OVERLAPPED overlapped;

    p_BindIoCompletionCallback = (void*)GetProcAddress(hmod, "BindIoCompletionCallback");
    if(!p_BindIoCompletionCallback) {
        win_skip("BindIoCompletionCallback not found in this DLL\n");
        return;
    }

    sem = CreateSemaphoreW(NULL, 0, 1, NULL);
    ok(sem != INVALID_HANDLE_VALUE, "Creating a semaphore failed\n");

    ret = GetTempPathA(MAX_PATH, temp_path);
    ok(ret != 0, "GetTempPathA error %ld\n", GetLastError());
    ok(ret < MAX_PATH, "temp path should fit into MAX_PATH\n");

    ret = GetTempFileNameA(temp_path, prefix, 0, filename);
    ok(ret != 0, "GetTempFileNameA error %ld\n", GetLastError());

    hFile = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_FLAG_RANDOM_ACCESS, 0);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileA: error %ld\n", GetLastError());

    retb = p_BindIoCompletionCallback(hFile, iocp_callback, 0);
    ok(retb == FALSE, "BindIoCompletionCallback succeeded on a file that wasn't created with FILE_FLAG_OVERLAPPED\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "Last error is %ld\n", GetLastError());

    ret = CloseHandle(hFile);
    ok( ret, "CloseHandle: error %ld\n", GetLastError());
    ret = DeleteFileA(filename);
    ok( ret, "DeleteFileA: error %ld\n", GetLastError());

    hFile = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, 0);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileA: error %ld\n", GetLastError());

    retb = p_BindIoCompletionCallback(hFile, iocp_callback, 0);
    ok(retb == TRUE, "BindIoCompletionCallback failed\n");

    memset(&overlapped, 0, sizeof(overlapped));
    retb = WriteFile(hFile, buffer, 4, &bytesWritten, &overlapped);
    ok(retb == TRUE || GetLastError() == ERROR_IO_PENDING, "WriteFile failed, lastError = %ld\n", GetLastError());

    ret = WaitForSingleObject(sem, 5000);
    ok(ret == WAIT_OBJECT_0, "Wait for the IO completion callback failed\n");
    CloseHandle(sem);

    retb = p_BindIoCompletionCallback(hFile, iocp_callback, 0);
    ok(retb == FALSE, "BindIoCompletionCallback succeeded when setting the same callback on the file again\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "Last error is %ld\n", GetLastError());
    retb = p_BindIoCompletionCallback(hFile, NULL, 0);
    ok(retb == FALSE, "BindIoCompletionCallback succeeded when setting the callback to NULL\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "Last error is %ld\n", GetLastError());

    ret = CloseHandle(hFile);
    ok( ret, "CloseHandle: error %ld\n", GetLastError());
    ret = DeleteFileA(filename);
    ok( ret, "DeleteFileA: error %ld\n", GetLastError());

    /* win2k3 requires the Flags parameter to be zero */
    SetLastError(0xdeadbeef);
    hFile = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, 0);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileA: error %ld\n", GetLastError());
    retb = p_BindIoCompletionCallback(hFile, iocp_callback, 12345);
    if (!retb)
        ok(GetLastError() == ERROR_INVALID_PARAMETER,
           "Expected ERROR_INVALID_PARAMETER, got %ld\n", GetLastError());
    else
        ok(retb == TRUE, "BindIoCompletionCallback failed with Flags != 0\n");
    ret = CloseHandle(hFile);
    ok( ret, "CloseHandle: error %ld\n", GetLastError());
    ret = DeleteFileA(filename);
    ok( ret, "DeleteFileA: error %ld\n", GetLastError());

    retb = p_BindIoCompletionCallback(NULL, iocp_callback, 0);
    ok(retb == FALSE, "BindIoCompletionCallback succeeded on a NULL file\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE ||
       GetLastError() == ERROR_INVALID_PARAMETER, /* vista */
       "Last error is %ld\n", GetLastError());
}

static void CALLBACK timer_queue_cb1(PVOID p, BOOLEAN timedOut)
{
    int *pn = p;
    ok(timedOut, "Timer callbacks should always time out\n");
    ++*pn;
}

struct timer_queue_data1
{
    int num_calls;
    int max_calls;
    HANDLE q, t;
};

static void CALLBACK timer_queue_cb2(PVOID p, BOOLEAN timedOut)
{
    struct timer_queue_data1 *d = p;
    ok(timedOut, "Timer callbacks should always time out\n");
    if (d->t && ++d->num_calls == d->max_calls)
    {
        BOOL ret;
        SetLastError(0xdeadbeef);
        /* Note, XP SP2 does *not* do any deadlock checking, so passing
           INVALID_HANDLE_VALUE here will just hang.  */
        ret = DeleteTimerQueueTimer(d->q, d->t, NULL);
        ok(!ret, "DeleteTimerQueueTimer\n");
        ok(GetLastError() == ERROR_IO_PENDING, "DeleteTimerQueueTimer\n");
    }
}

static void CALLBACK timer_queue_cb3(PVOID p, BOOLEAN timedOut)
{
    struct timer_queue_data1 *d = p;
    ok(timedOut, "Timer callbacks should always time out\n");
    if (d->t && ++d->num_calls == d->max_calls)
    {
        /* Basically kill the timer since it won't have time to run
           again.  */
        BOOL ret = ChangeTimerQueueTimer(d->q, d->t, 10000, 0);
        ok(ret, "ChangeTimerQueueTimer\n");
    }
}

static void CALLBACK timer_queue_cb4(PVOID p, BOOLEAN timedOut)
{
    struct timer_queue_data1 *d = p;
    ok(timedOut, "Timer callbacks should always time out\n");
    if (d->t)
    {
        /* This tests whether a timer gets flagged for deletion before
           or after the callback runs.  If we start this timer with a
           period of zero (run once), then ChangeTimerQueueTimer will
           fail if the timer is already flagged.  Hence we really run
           only once.  Otherwise we will run multiple times.  */
        BOOL ret = ChangeTimerQueueTimer(d->q, d->t, 50, 50);
        ok(ret, "ChangeTimerQueueTimer\n");
        ++d->num_calls;
    }
}

static void CALLBACK timer_queue_cb5(PVOID p, BOOLEAN timedOut)
{
    DWORD_PTR delay = (DWORD_PTR) p;
    ok(timedOut, "Timer callbacks should always time out\n");
    if (delay)
        Sleep(delay);
}

static void CALLBACK timer_queue_cb6(PVOID p, BOOLEAN timedOut)
{
    struct timer_queue_data1 *d = p;
    ok(timedOut, "Timer callbacks should always time out\n");
    /* This tests an original implementation bug where a deleted timer may get
       to run, but it is tricky to set up.  */
    if (d->q && d->num_calls++ == 0)
    {
        /* First run: delete ourselves, then insert and remove a timer
           that goes in front of us in the sorted timeout list.  Once
           removed, we will still timeout at the faster timer's due time,
           but this should be a no-op if we are bug-free.  There should
           not be a second run.  We can test the value of num_calls later.  */
        BOOL ret;
        HANDLE t;

        /* The delete will pend while we are in this callback.  */
        SetLastError(0xdeadbeef);
        ret = DeleteTimerQueueTimer(d->q, d->t, NULL);
        ok(!ret, "DeleteTimerQueueTimer\n");
        ok(GetLastError() == ERROR_IO_PENDING, "DeleteTimerQueueTimer\n");

        ret = CreateTimerQueueTimer(&t, d->q, timer_queue_cb1, NULL, 100, 0, 0);
        ok(ret, "CreateTimerQueueTimer\n");
        ok(t != NULL, "CreateTimerQueueTimer\n");

        ret = DeleteTimerQueueTimer(d->q, t, INVALID_HANDLE_VALUE);
        ok(ret, "DeleteTimerQueueTimer\n");

        /* Now we stay alive by hanging around in the callback.  */
        Sleep(500);
    }
}

static void test_timer_queue(void)
{
    HANDLE q, t0, t1, t2, t3, t4, t5;
    int n0, n1, n2, n3, n4, n5;
    struct timer_queue_data1 d1, d2, d3, d4;
    HANDLE e, et1, et2;
    BOOL ret, ret0;

    /* Test asynchronous deletion of the queue. */
    q = CreateTimerQueue();
    ok(q != NULL, "CreateTimerQueue\n");

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueEx(q, NULL);
    ok(ret /* vista */ || GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueEx, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());

    /* Test synchronous deletion of the queue and running timers. */
    q = CreateTimerQueue();
    ok(q != NULL, "CreateTimerQueue\n");

    /* Not called. */
    t0 = NULL;
    n0 = 0;
    ret = CreateTimerQueueTimer(&t0, q, timer_queue_cb1, &n0, 0, 300, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t0 != NULL, "CreateTimerQueueTimer\n");
    ret0 = DeleteTimerQueueTimer(q, t0, NULL);
    ok((!ret0 && GetLastError() == ERROR_IO_PENDING) ||
       broken(ret0), /* Win 2000 & XP & 2003 */
       "DeleteTimerQueueTimer ret=%d le=%lu\n", ret0, GetLastError());

    /* Called once.  */
    t1 = NULL;
    n1 = 0;
    ret = CreateTimerQueueTimer(&t1, q, timer_queue_cb1, &n1, 0, 0, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t1 != NULL, "CreateTimerQueueTimer\n");

    /* A slow one.  */
    t2 = NULL;
    n2 = 0;
    ret = CreateTimerQueueTimer(&t2, q, timer_queue_cb1, &n2, 0, 100, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t2 != NULL, "CreateTimerQueueTimer\n");

    /* A fast one.  */
    t3 = NULL;
    n3 = 0;
    ret = CreateTimerQueueTimer(&t3, q, timer_queue_cb1, &n3, 0, 10, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t3 != NULL, "CreateTimerQueueTimer\n");

    /* Start really late (it won't start).  */
    t4 = NULL;
    n4 = 0;
    ret = CreateTimerQueueTimer(&t4, q, timer_queue_cb1, &n4, 10000, 10, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t4 != NULL, "CreateTimerQueueTimer\n");

    /* Start soon, but delay so long it won't run again.  */
    t5 = NULL;
    n5 = 0;
    ret = CreateTimerQueueTimer(&t5, q, timer_queue_cb1, &n5, 0, 10000, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t5 != NULL, "CreateTimerQueueTimer\n");

    /* Give them a chance to do some work.  */
    Sleep(500);

    /* Test deleting a once-only timer.  */
    ret = DeleteTimerQueueTimer(q, t1, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueTimer\n");

    /* A periodic timer.  */
    ret = DeleteTimerQueueTimer(q, t2, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueTimer\n");

    ret = DeleteTimerQueueEx(q, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueEx\n");
    todo_wine
    ok(n0 == 1 || broken(ret0 && n0 == 0), "Timer callback 0 expected 1 got %d\n", n0);
    ok(n1 == 1, "Timer callback 1 expected 1 got %d\n", n1);
    ok(n2 < n3, "Timer callback 2 & 3 expected %d < %d\n", n2, n3);
    ok(n4 == 0, "Timer callback 4 expected 0 got %d\n", n4);
    ok(n5 == 1, "Timer callback 5 expected 1 got %d\n", n5);

    /* Test synchronous deletion of the timer/queue with event trigger. */
    e = CreateEventW(NULL, TRUE, FALSE, NULL);
    et1 = CreateEventW(NULL, TRUE, FALSE, NULL);
    et2 = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!e || !et1 || !et2)
    {
        skip("Failed to create timer queue descruction event\n");
        return;
    }

    q = CreateTimerQueue();
    ok(q != NULL, "CreateTimerQueue\n");

    /* Run once and finish quickly (should be done when we delete it).  */
    t1 = NULL;
    ret = CreateTimerQueueTimer(&t1, q, timer_queue_cb5, NULL, 0, 0, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t1 != NULL, "CreateTimerQueueTimer\n");

    /* Run once and finish slowly (shouldn't be done when we delete it).  */
    t2 = NULL;
    ret = CreateTimerQueueTimer(&t2, q, timer_queue_cb5, (PVOID) 1000, 0, 0, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t2 != NULL, "CreateTimerQueueTimer\n");

    /* Run once and finish quickly (should be done when we delete it).  */
    t3 = NULL;
    ret = CreateTimerQueueTimer(&t3, q, timer_queue_cb5, NULL, 0, 0, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t3 != NULL, "CreateTimerQueueTimer\n");

    /* Run once and finish slowly (shouldn't be done when we delete it).  */
    t4 = NULL;
    ret = CreateTimerQueueTimer(&t4, q, timer_queue_cb5, (PVOID) 1000, 0, 0, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t4 != NULL, "CreateTimerQueueTimer\n");

    /* Give them a chance to start.  */
    Sleep(400);

    /* DeleteTimerQueueTimer always returns PENDING with a NULL event,
       even if the timer is finished.  */
    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueTimer(q, t1, NULL);
    ok(ret /* vista */ || GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueTimer, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueTimer(q, t2, NULL);
    ok(!ret, "DeleteTimerQueueTimer call was expected to fail\n");
    ok(GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueTimer, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueTimer(q, t3, et1);
    ok(ret, "DeleteTimerQueueTimer call was expected to fail\n");
    ok(GetLastError() == 0xdeadbeef,
       "DeleteTimerQueueTimer, GetLastError: expected 0xdeadbeef, got %ld\n",
       GetLastError());
    ok(WaitForSingleObject(et1, 250) == WAIT_OBJECT_0,
       "Timer destruction event not triggered\n");

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueTimer(q, t4, et2);
    ok(!ret, "DeleteTimerQueueTimer call was expected to fail\n");
    ok(GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueTimer, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());
    ok(WaitForSingleObject(et2, 1000) == WAIT_OBJECT_0,
       "Timer destruction event not triggered\n");

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueEx(q, e);
    ok(ret /* vista */ || GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueEx, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());
    ok(WaitForSingleObject(e, 250) == WAIT_OBJECT_0,
       "Queue destruction event not triggered\n");
    CloseHandle(e);

    /* Test deleting/changing a timer in execution.  */
    q = CreateTimerQueue();
    ok(q != NULL, "CreateTimerQueue\n");

    /* Test changing a once-only timer before it fires (this is allowed,
       whereas after it fires you cannot).  */
    n1 = 0;
    ret = CreateTimerQueueTimer(&t1, q, timer_queue_cb1, &n1, 10000, 0, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t1 != NULL, "CreateTimerQueueTimer\n");
    ret = ChangeTimerQueueTimer(q, t1, 0, 0);
    ok(ret, "ChangeTimerQueueTimer\n");

    d2.t = t2 = NULL;
    d2.num_calls = 0;
    d2.max_calls = 3;
    d2.q = q;
    ret = CreateTimerQueueTimer(&t2, q, timer_queue_cb2, &d2, 10, 10, 0);
    d2.t = t2;
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t2 != NULL, "CreateTimerQueueTimer\n");

    d3.t = t3 = NULL;
    d3.num_calls = 0;
    d3.max_calls = 4;
    d3.q = q;
    ret = CreateTimerQueueTimer(&t3, q, timer_queue_cb3, &d3, 10, 10, 0);
    d3.t = t3;
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t3 != NULL, "CreateTimerQueueTimer\n");

    d4.t = t4 = NULL;
    d4.num_calls = 0;
    d4.q = q;
    ret = CreateTimerQueueTimer(&t4, q, timer_queue_cb4, &d4, 10, 0, 0);
    d4.t = t4;
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t4 != NULL, "CreateTimerQueueTimer\n");

    Sleep(500);

    ret = DeleteTimerQueueEx(q, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueEx\n");
    ok(n1 == 1, "ChangeTimerQueueTimer\n");
    ok(d2.num_calls == d2.max_calls, "DeleteTimerQueueTimer\n");
    ok(d3.num_calls == d3.max_calls, "ChangeTimerQueueTimer\n");
    ok(d4.num_calls == 1, "Timer flagged for deletion incorrectly\n");

    /* Test an obscure bug that was in the original implementation.  */
    q = CreateTimerQueue();
    ok(q != NULL, "CreateTimerQueue\n");

    /* All the work is done in the callback.  */
    d1.t = t1 = NULL;
    d1.num_calls = 0;
    d1.q = q;
    ret = CreateTimerQueueTimer(&t1, q, timer_queue_cb6, &d1, 100, 100, WT_EXECUTELONGFUNCTION);
    d1.t = t1;
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t1 != NULL, "CreateTimerQueueTimer\n");

    Sleep(750);

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueEx(q, NULL);
    ok(ret /* vista */ || GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueEx, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());
    ok(d1.num_calls == 1, "DeleteTimerQueueTimer\n");

    /* Test functions on the default timer queue.  */
    t1 = NULL;
    n1 = 0;
    ret = CreateTimerQueueTimer(&t1, NULL, timer_queue_cb1, &n1, 1000, 1000, 0);
    ok(ret, "CreateTimerQueueTimer, default queue\n");
    ok(t1 != NULL, "CreateTimerQueueTimer, default queue\n");

    ret = ChangeTimerQueueTimer(NULL, t1, 2000, 2000);
    ok(ret, "ChangeTimerQueueTimer, default queue\n");

    ret = DeleteTimerQueueTimer(NULL, t1, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueTimer, default queue\n");

    /* Try mixing default and non-default queues.  Apparently this works.  */
    q = CreateTimerQueue();
    ok(q != NULL, "CreateTimerQueue\n");

    t1 = NULL;
    n1 = 0;
    ret = CreateTimerQueueTimer(&t1, q, timer_queue_cb1, &n1, 1000, 1000, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t1 != NULL, "CreateTimerQueueTimer\n");

    t2 = NULL;
    n2 = 0;
    ret = CreateTimerQueueTimer(&t2, NULL, timer_queue_cb1, &n2, 1000, 1000, 0);
    ok(ret, "CreateTimerQueueTimer\n");
    ok(t2 != NULL, "CreateTimerQueueTimer\n");

    ret = ChangeTimerQueueTimer(NULL, t1, 2000, 2000);
    ok(ret, "ChangeTimerQueueTimer\n");

    ret = ChangeTimerQueueTimer(q, t2, 2000, 2000);
    ok(ret, "ChangeTimerQueueTimer\n");

    ret = DeleteTimerQueueTimer(NULL, t1, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueTimer\n");

    ret = DeleteTimerQueueTimer(q, t2, INVALID_HANDLE_VALUE);
    ok(ret, "DeleteTimerQueueTimer\n");

    /* Try to delete the default queue?  In any case: not allowed.  */
    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueEx(NULL, NULL);
    ok(!ret, "DeleteTimerQueueEx call was expected to fail\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE,
       "DeleteTimerQueueEx, GetLastError: expected ERROR_INVALID_HANDLE, got %ld\n",
       GetLastError());

    SetLastError(0xdeadbeef);
    ret = DeleteTimerQueueEx(q, NULL);
    ok(ret /* vista */ || GetLastError() == ERROR_IO_PENDING,
       "DeleteTimerQueueEx, GetLastError: expected ERROR_IO_PENDING, got %ld\n",
       GetLastError());
}

static HANDLE modify_handle(HANDLE handle, DWORD modify)
{
    DWORD tmp = HandleToULong(handle);
    tmp |= modify;
    return ULongToHandle(tmp);
}

static void test_WaitForSingleObject(void)
{
    HANDLE signaled, nonsignaled, invalid;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    DWORD ret;

    signaled = CreateEventW(NULL, TRUE, TRUE, NULL);
    nonsignaled = CreateEventW(NULL, TRUE, FALSE, NULL);
    invalid = (HANDLE) 0xdeadbee0;

    /* invalid handle with different values for lower 2 bits */
    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(invalid, 0);
    ok(ret == WAIT_FAILED, "expected WAIT_FAILED, got %ld\n", ret);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "expected ERROR_INVALID_HANDLE, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(invalid, 1), 0);
    ok(ret == WAIT_FAILED, "expected WAIT_FAILED, got %ld\n", ret);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "expected ERROR_INVALID_HANDLE, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(invalid, 2), 0);
    ok(ret == WAIT_FAILED, "expected WAIT_FAILED, got %ld\n", ret);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "expected ERROR_INVALID_HANDLE, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(invalid, 3), 0);
    ok(ret == WAIT_FAILED, "expected WAIT_FAILED, got %ld\n", ret);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "expected ERROR_INVALID_HANDLE, got %ld\n", GetLastError());

    /* valid handle with different values for lower 2 bits */
    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(nonsignaled, 0);
    ok(ret == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(nonsignaled, 1), 0);
    ok(ret == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(nonsignaled, 2), 0);
    ok(ret == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(nonsignaled, 3), 0);
    ok(ret == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    /* valid handle with different values for lower 2 bits */
    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(signaled, 0);
    ok(ret == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(signaled, 1), 0);
    ok(ret == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(signaled, 2), 0);
    ok(ret == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = WaitForSingleObject(modify_handle(signaled, 3), 0);
    ok(ret == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %ld\n", ret);
    ok(GetLastError() == 0xdeadbeef, "expected 0xdeadbeef, got %ld\n", GetLastError());

    /* pseudo handles are allowed in WaitForSingleObject and NtWaitForSingleObject */
    ret = WaitForSingleObject(GetCurrentProcess(), 100);
    ok(ret == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %lu\n", ret);

    ret = WaitForSingleObject(GetCurrentThread(), 100);
    ok(ret == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %lu\n", ret);

    timeout.QuadPart = -1000000;
    status = pNtWaitForSingleObject(GetCurrentProcess(), FALSE, &timeout);
    ok(status == STATUS_TIMEOUT, "expected STATUS_TIMEOUT, got %08lx\n", status);

    timeout.QuadPart = -1000000;
    status = pNtWaitForSingleObject(GetCurrentThread(), FALSE, &timeout);
    ok(status == STATUS_TIMEOUT, "expected STATUS_TIMEOUT, got %08lx\n", status);

    CloseHandle(signaled);
    CloseHandle(nonsignaled);
}

static void test_WaitForMultipleObjects(void)
{
    LARGE_INTEGER timeout;
    NTSTATUS status;
    DWORD r;
    int i;
    HANDLE maxevents[MAXIMUM_WAIT_OBJECTS];

    /* create the maximum number of events and make sure
     * we can wait on that many */
    for (i=0; i<MAXIMUM_WAIT_OBJECTS; i++)
    {
        maxevents[i] = CreateEventW(NULL, i==0, TRUE, NULL);
        ok( maxevents[i] != 0, "should create enough events\n");
    }

    /* a manual-reset event remains signaled, an auto-reset event is cleared */
    r = WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, maxevents, FALSE, 0);
    ok( r == WAIT_OBJECT_0, "should signal lowest handle first, got %ld\n", r);
    r = WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, maxevents, FALSE, 0);
    ok( r == WAIT_OBJECT_0, "should signal handle #0 first, got %ld\n", r);
    ok(ResetEvent(maxevents[0]), "ResetEvent\n");
    for (i=1; i<MAXIMUM_WAIT_OBJECTS; i++)
    {
        /* the lowest index is checked first and remaining events are untouched */
        r = WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, maxevents, FALSE, 0);
        ok( r == WAIT_OBJECT_0+i, "should signal handle #%d first, got %ld\n", i, r);
    }

    /* run same test with Nt* call */
    for (i=0; i<MAXIMUM_WAIT_OBJECTS; i++)
        SetEvent(maxevents[i]);

    /* a manual-reset event remains signaled, an auto-reset event is cleared */
    status = pNtWaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, maxevents, TRUE, FALSE, NULL);
    ok(status == STATUS_WAIT_0, "should signal lowest handle first, got %08lx\n", status);
    status = pNtWaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, maxevents, TRUE, FALSE, NULL);
    ok(status == STATUS_WAIT_0, "should signal handle #0 first, got %08lx\n", status);
    ok(ResetEvent(maxevents[0]), "ResetEvent\n");
    for (i=1; i<MAXIMUM_WAIT_OBJECTS; i++)
    {
        /* the lowest index is checked first and remaining events are untouched */
        status = pNtWaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, maxevents, TRUE, FALSE, NULL);
        ok(status == STATUS_WAIT_0 + i, "should signal handle #%d first, got %08lx\n", i, status);
    }

    for (i=0; i<MAXIMUM_WAIT_OBJECTS; i++)
        if (maxevents[i]) CloseHandle(maxevents[i]);

    /* in contrast to WaitForSingleObject, pseudo handles are not allowed in
     * WaitForMultipleObjects and NtWaitForMultipleObjects */
    maxevents[0] = GetCurrentProcess();
    SetLastError(0xdeadbeef);
    r = WaitForMultipleObjects(1, maxevents, FALSE, 100);
    todo_wine ok(r == WAIT_FAILED, "expected WAIT_FAILED, got %lu\n", r);
    todo_wine ok(GetLastError() == ERROR_INVALID_HANDLE,
                 "expected ERROR_INVALID_HANDLE, got %lu\n", GetLastError());

    maxevents[0] = GetCurrentThread();
    SetLastError(0xdeadbeef);
    r = WaitForMultipleObjects(1, maxevents, FALSE, 100);
    todo_wine ok(r == WAIT_FAILED, "expected WAIT_FAILED, got %lu\n", r);
    todo_wine ok(GetLastError() == ERROR_INVALID_HANDLE,
                 "expected ERROR_INVALID_HANDLE, got %lu\n", GetLastError());

    timeout.QuadPart = -1000000;
    maxevents[0] = GetCurrentProcess();
    status = pNtWaitForMultipleObjects(1, maxevents, TRUE, FALSE, &timeout);
    todo_wine ok(status == STATUS_INVALID_HANDLE, "expected STATUS_INVALID_HANDLE, got %08lx\n", status);

    timeout.QuadPart = -1000000;
    maxevents[0] = GetCurrentThread();
    status = pNtWaitForMultipleObjects(1, maxevents, TRUE, FALSE, &timeout);
    todo_wine ok(status == STATUS_INVALID_HANDLE, "expected STATUS_INVALID_HANDLE, got %08lx\n", status);
}

static BOOL g_initcallback_ret, g_initcallback_called;
static void *g_initctxt;

static BOOL CALLBACK initonce_callback(INIT_ONCE *initonce, void *parameter, void **ctxt)
{
    g_initcallback_called = TRUE;
    /* zero bit set means here that initialization is taking place - initialization locked */
    ok(g_initctxt == *ctxt, "got wrong context value %p, expected %p\n", *ctxt, g_initctxt);
    ok(initonce->Ptr == (void*)0x1, "got %p\n", initonce->Ptr);
    ok(parameter == (void*)0xdeadbeef, "got wrong parameter\n");
    return g_initcallback_ret;
}

static void test_initonce(void)
{
    INIT_ONCE initonce;
    BOOL ret, pending;

    if (!pInitOnceInitialize || !pInitOnceExecuteOnce)
    {
        win_skip("one-time initialization API not supported\n");
        return;
    }

    /* blocking initialization with callback */
    initonce.Ptr = (void*)0xdeadbeef;
    pInitOnceInitialize(&initonce);
    ok(initonce.Ptr == NULL, "got %p\n", initonce.Ptr);

    /* initialisation completed successfully */
    g_initcallback_ret = TRUE;
    g_initctxt = NULL;
    ret = pInitOnceExecuteOnce(&initonce, initonce_callback, (void*)0xdeadbeef, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0x2, "got %p\n", initonce.Ptr);
    ok(g_initctxt == NULL, "got %p\n", g_initctxt);
    ok(g_initcallback_called, "got %d\n", g_initcallback_called);

    /* so it's been called already so won't be called again */
    g_initctxt = NULL;
    g_initcallback_called = FALSE;
    ret = pInitOnceExecuteOnce(&initonce, initonce_callback, (void*)0xdeadbeef, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0x2, "got %p\n", initonce.Ptr);
    ok(g_initctxt == NULL, "got %p\n", g_initctxt);
    ok(!g_initcallback_called, "got %d\n", g_initcallback_called);

    pInitOnceInitialize(&initonce);
    g_initcallback_called = FALSE;
    /* 2 lower order bits should never be used, you'll get a crash in result */
    g_initctxt = (void*)0xFFFFFFF0;
    ret = pInitOnceExecuteOnce(&initonce, initonce_callback, (void*)0xdeadbeef, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0xFFFFFFF2, "got %p\n", initonce.Ptr);
    ok(g_initctxt == (void*)0xFFFFFFF0, "got %p\n", g_initctxt);
    ok(g_initcallback_called, "got %d\n", g_initcallback_called);

    /* callback failed */
    g_initcallback_ret = FALSE;
    g_initcallback_called = FALSE;
    g_initctxt = NULL;
    pInitOnceInitialize(&initonce);
    SetLastError( 0xdeadbeef );
    ret = pInitOnceExecuteOnce(&initonce, initonce_callback, (void*)0xdeadbeef, &g_initctxt);
    ok(!ret && GetLastError() == 0xdeadbeef, "got wrong ret value %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == NULL, "got %p\n", initonce.Ptr);
    ok(g_initctxt == NULL, "got %p\n", g_initctxt);
    ok(g_initcallback_called, "got %d\n", g_initcallback_called);

    /* blocking initialization without a callback */
    pInitOnceInitialize(&initonce);
    g_initctxt = NULL;
    pending = FALSE;
    ret = pInitOnceBeginInitialize(&initonce, 0, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);
    ok(g_initctxt == NULL, "got %p\n", g_initctxt);
    /* another attempt to begin initialization with block a single thread */

    g_initctxt = NULL;
    pending = 0xf;
    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_GEN_FAILURE, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending == 0xf, "got %d\n", pending);
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);
    ok(g_initctxt == NULL, "got %p\n", g_initctxt);

    g_initctxt = (void*)0xdeadbee0;
    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED, g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);

    /* once failed already */
    g_initctxt = (void*)0xdeadbee0;
    ret = pInitOnceComplete(&initonce, 0, g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);

    pInitOnceInitialize(&initonce);
    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED, NULL);
    ok(!ret && GetLastError() == ERROR_GEN_FAILURE, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == NULL, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED | INIT_ONCE_ASYNC, NULL);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == NULL, "got %p\n", initonce.Ptr);

    ret = pInitOnceBeginInitialize(&initonce, 0, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED | INIT_ONCE_ASYNC, NULL);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, 0, (void *)0xdeadbeef);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);

    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED, NULL);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == NULL, "got %p\n", initonce.Ptr);

    pInitOnceInitialize(&initonce);
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)3, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, 0, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());

    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)3, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED, NULL);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)3, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED | INIT_ONCE_ASYNC, NULL);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)3, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_ASYNC, (void *)0xdeadbeef);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)3, "got %p\n", initonce.Ptr);

    ret = pInitOnceComplete(&initonce, INIT_ONCE_ASYNC, (void *)0xdeadbee0);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceComplete(&initonce, INIT_ONCE_INIT_FAILED | INIT_ONCE_ASYNC, NULL);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);

    pInitOnceInitialize(&initonce);
    ret = pInitOnceBeginInitialize(&initonce, 0, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);

    /* test INIT_ONCE_CHECK_ONLY */

    pInitOnceInitialize(&initonce);
    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_GEN_FAILURE, "wrong ret %d err %lu\n", ret, GetLastError());
    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY|INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());

    ret = pInitOnceBeginInitialize(&initonce, 0, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)1, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_GEN_FAILURE, "wrong ret %d err %lu\n", ret, GetLastError());
    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY|INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());

    ret = pInitOnceComplete(&initonce, 0, (void *)0xdeadbee0);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);

    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY, &pending, &g_initctxt);
    ok(ret, "got wrong ret value %d err %lu\n", ret, GetLastError());
    ok(!pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);
    ok(g_initctxt == (void*)0xdeadbee0, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY|INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());

    pInitOnceInitialize(&initonce);
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)3, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_GEN_FAILURE, "wrong ret %d err %lu\n", ret, GetLastError());
    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY|INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());

    ret = pInitOnceComplete(&initonce, INIT_ONCE_ASYNC, (void *)0xdeadbee0);
    ok(ret, "wrong ret %d err %lu\n", ret, GetLastError());
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);

    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY, &pending, &g_initctxt);
    ok(ret, "got wrong ret value %d err %lu\n", ret, GetLastError());
    ok(!pending, "got %d\n", pending);
    ok(initonce.Ptr == (void*)0xdeadbee2, "got %p\n", initonce.Ptr);
    ok(g_initctxt == (void*)0xdeadbee0, "got %p\n", initonce.Ptr);

    SetLastError( 0xdeadbeef );
    ret = pInitOnceBeginInitialize(&initonce, INIT_ONCE_CHECK_ONLY|INIT_ONCE_ASYNC, &pending, &g_initctxt);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "wrong ret %d err %lu\n", ret, GetLastError());
}

static CONDITION_VARIABLE buffernotempty = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE buffernotfull = CONDITION_VARIABLE_INIT;
static CRITICAL_SECTION   buffercrit;
static BOOL condvar_stop = FALSE, condvar_sleeperr = FALSE;
static LONG bufferlen,totalproduced,totalconsumed;
static LONG condvar_producer_sleepcnt,condvar_consumer_sleepcnt;

#define BUFFER_SIZE 5

static DWORD WINAPI condvar_producer(LPVOID x) {
    DWORD sleepinterval = 5;

    while (1) {
        Sleep(sleepinterval);
        if (sleepinterval > 1)
            sleepinterval -= 1;

        EnterCriticalSection(&buffercrit);
        while ((bufferlen == BUFFER_SIZE) && !condvar_stop) {
            condvar_producer_sleepcnt++;
            if (!pSleepConditionVariableCS(&buffernotfull, &buffercrit, sleepinterval)) {
                if (GetLastError() != ERROR_TIMEOUT)
                    condvar_sleeperr = TRUE;
            }
        }
        if (condvar_stop) {
            LeaveCriticalSection(&buffercrit);
            break;
        }
        bufferlen++;
        totalproduced++;
        LeaveCriticalSection(&buffercrit);
        pWakeConditionVariable(&buffernotempty);
    }
    return 0;
}

static DWORD WINAPI condvar_consumer(LPVOID x) {
    DWORD *cnt = (DWORD*)x;
    DWORD sleepinterval = 1;

    while (1) {
        EnterCriticalSection(&buffercrit);
        while ((bufferlen == 0) && !condvar_stop) {
            condvar_consumer_sleepcnt++;
            if (!pSleepConditionVariableCS (&buffernotempty, &buffercrit, sleepinterval)) {
                if (GetLastError() != ERROR_TIMEOUT)
                    condvar_sleeperr = TRUE;
            }
        }
        if (condvar_stop && (bufferlen == 0)) {
            LeaveCriticalSection(&buffercrit);
            break;
        }
        bufferlen--;
        totalconsumed++;
        (*cnt)++;
        LeaveCriticalSection(&buffercrit);
        pWakeConditionVariable(&buffernotfull);
        Sleep(sleepinterval);
        if (sleepinterval < 5) sleepinterval += 1;
    }
    return 0;
}

static void test_condvars_consumer_producer(void)
{
    HANDLE hp1,hp2,hp3,hc1,hc2,hc3;
    DWORD dummy;
    DWORD cnt1,cnt2,cnt3;

    if (!pInitializeConditionVariable) {
        /* function is not yet in XP, only in newer Windows */
        win_skip("no condition variable support.\n");
        return;
    }

    /* Implement a producer / consumer scheme with non-full / non-empty triggers */

    /* If we have static initialized condition variables, InitializeConditionVariable
     * is not strictly necessary.
     * pInitializeConditionVariable(&buffernotfull);
     */
    pInitializeConditionVariable(&buffernotempty);
    InitializeCriticalSection(&buffercrit);

    /* Larger Test: consumer/producer example */

    bufferlen = totalproduced = totalconsumed = cnt1 = cnt2 = cnt3 = 0;

    hp1 = CreateThread(NULL, 0, condvar_producer, NULL, 0, &dummy);
    hp2 = CreateThread(NULL, 0, condvar_producer, NULL, 0, &dummy);
    hp3 = CreateThread(NULL, 0, condvar_producer, NULL, 0, &dummy);
    hc1 = CreateThread(NULL, 0, condvar_consumer, (PVOID)&cnt1, 0, &dummy);
    hc2 = CreateThread(NULL, 0, condvar_consumer, (PVOID)&cnt2, 0, &dummy);
    hc3 = CreateThread(NULL, 0, condvar_consumer, (PVOID)&cnt3, 0, &dummy);

    /* Limit run to 0.5 seconds. */
    Sleep(500);

    /* tear down start */
    condvar_stop = TRUE;

    /* final wake up call */
    pWakeAllConditionVariable (&buffernotfull);
    pWakeAllConditionVariable (&buffernotempty);

    /* (mostly an implementation detail)
     * ok(buffernotfull.Ptr == NULL, "buffernotfull.Ptr is %p\n", buffernotfull.Ptr);
     */

    WaitForSingleObject(hp1, 1000);
    WaitForSingleObject(hp2, 1000);
    WaitForSingleObject(hp3, 1000);
    WaitForSingleObject(hc1, 1000);
    WaitForSingleObject(hc2, 1000);
    WaitForSingleObject(hc3, 1000);

    ok(totalconsumed == totalproduced,
       "consumed %ld != produced %ld\n", totalconsumed, totalproduced);
    ok (!condvar_sleeperr, "error occurred during SleepConditionVariableCS\n");

    /* Checking cnt1 - cnt2 for non-0 would be not good, the case where
     * one consumer does not get anything to do is possible. */
    trace("produced %ld, c1 %ld, c2 %ld, c3 %ld\n", totalproduced, cnt1, cnt2, cnt3);
    /* The sleeps of the producer or consumer should not go above 100* produced count,
     * otherwise the implementation does not sleep correctly. But yet again, this is
     * not hard defined. */
    trace("producer sleep %ld, consumer sleep %ld\n", condvar_producer_sleepcnt, condvar_consumer_sleepcnt);
}

/* Sample test for some sequence of events happening, sequenced using "condvar_seq" */
static DWORD condvar_seq = 0;
static CONDITION_VARIABLE aligned_cv;
static CRITICAL_SECTION condvar_crit;
static SRWLOCK condvar_srwlock;

#pragma pack(push,1)
static struct
{
    char c;
    CONDITION_VARIABLE cv;
} unaligned_cv;
#pragma pack(pop)

/* Sequence of wake/sleep to check boundary conditions:
 * 0: init
 * 1: producer emits a WakeConditionVariable without consumer waiting.
 * 2: consumer sleeps without a wake expecting timeout
 * 3: producer emits a WakeAllConditionVariable without consumer waiting.
 * 4: consumer sleeps without a wake expecting timeout
 * 5: a wake is handed to a SleepConditionVariableCS
 * 6: a wakeall is handed to a SleepConditionVariableCS
 * 7: sleep after above should timeout
 * 8: wake with crit section locked into the sleep timeout
 *
 * the following tests will only be executed if InitializeSRWLock is available
 *
 *  9: producer (exclusive) wakes up consumer (exclusive)
 * 10: producer (exclusive) wakes up consumer (shared)
 * 11: producer (shared) wakes up consumer (exclusive)
 * 12: producer (shared) wakes up consumer (shared)
 * 13: end
 */
static DWORD WINAPI condvar_base_producer(void *arg)
{
    CONDITION_VARIABLE *cv = arg;

    while (condvar_seq < 1) Sleep(1);

    pWakeConditionVariable(cv);
    condvar_seq = 2;

    while (condvar_seq < 3) Sleep(1);
    pWakeAllConditionVariable(cv);
    condvar_seq = 4;

    while (condvar_seq < 5) Sleep(1);
    EnterCriticalSection (&condvar_crit);
    pWakeConditionVariable(cv);
    LeaveCriticalSection (&condvar_crit);
    while (condvar_seq < 6) Sleep(1);
    EnterCriticalSection (&condvar_crit);
    pWakeAllConditionVariable(cv);
    LeaveCriticalSection (&condvar_crit);

    while (condvar_seq < 8) Sleep(1);
    EnterCriticalSection (&condvar_crit);
    pWakeConditionVariable(cv);
    Sleep(50);
    LeaveCriticalSection (&condvar_crit);

    /* skip over remaining tests if InitializeSRWLock is not available */
    if (!pInitializeSRWLock)
        return 0;

    while (condvar_seq < 9) Sleep(1);
    pAcquireSRWLockExclusive(&condvar_srwlock);
    pWakeConditionVariable(cv);
    pReleaseSRWLockExclusive(&condvar_srwlock);

    while (condvar_seq < 10) Sleep(1);
    pAcquireSRWLockExclusive(&condvar_srwlock);
    pWakeConditionVariable(cv);
    pReleaseSRWLockExclusive(&condvar_srwlock);

    while (condvar_seq < 11) Sleep(1);
    pAcquireSRWLockShared(&condvar_srwlock);
    pWakeConditionVariable(cv);
    pReleaseSRWLockShared(&condvar_srwlock);

    while (condvar_seq < 12) Sleep(1);
    Sleep(50); /* ensure that consumer waits for cond variable */
    pAcquireSRWLockShared(&condvar_srwlock);
    pWakeConditionVariable(cv);
    pReleaseSRWLockShared(&condvar_srwlock);

    return 0;
}

static DWORD WINAPI condvar_base_consumer(void *arg)
{
    CONDITION_VARIABLE *cv = arg;
    BOOL ret;

    while (condvar_seq < 2) Sleep(1);

    /* wake was emitted, but we were not sleeping */
    EnterCriticalSection (&condvar_crit);
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 10);
    LeaveCriticalSection (&condvar_crit);
    ok (!ret, "SleepConditionVariableCS should return FALSE on out of band wake\n");
    ok (GetLastError() == ERROR_TIMEOUT, "SleepConditionVariableCS should return ERROR_TIMEOUT on out of band wake, not %ld\n", GetLastError());

    condvar_seq = 3;
    while (condvar_seq < 4) Sleep(1);

    /* wake all was emitted, but we were not sleeping */
    EnterCriticalSection (&condvar_crit);
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 10);
    LeaveCriticalSection (&condvar_crit);
    ok (!ret, "SleepConditionVariableCS should return FALSE on out of band wake\n");
    ok (GetLastError() == ERROR_TIMEOUT, "SleepConditionVariableCS should return ERROR_TIMEOUT on out of band wake, not %ld\n", GetLastError());

    EnterCriticalSection (&condvar_crit);
    condvar_seq = 5;
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 200);
    LeaveCriticalSection (&condvar_crit);
    ok (ret, "SleepConditionVariableCS should return TRUE on good wake\n");

    EnterCriticalSection (&condvar_crit);
    condvar_seq = 6;
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 200);
    LeaveCriticalSection (&condvar_crit);
    ok (ret, "SleepConditionVariableCS should return TRUE on good wakeall\n");
    condvar_seq = 7;

    EnterCriticalSection (&condvar_crit);
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 10);
    LeaveCriticalSection (&condvar_crit);
    ok (!ret, "SleepConditionVariableCS should return FALSE on out of band wake\n");
    ok (GetLastError() == ERROR_TIMEOUT, "SleepConditionVariableCS should return ERROR_TIMEOUT on out of band wake, not %ld\n", GetLastError());

    EnterCriticalSection (&condvar_crit);
    condvar_seq = 8;
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 20);
    LeaveCriticalSection (&condvar_crit);
    ok (ret, "SleepConditionVariableCS should still return TRUE on crit unlock delay\n");

    /* skip over remaining tests if InitializeSRWLock is not available */
    if (!pInitializeSRWLock)
    {
        win_skip("no srw lock support.\n");
        condvar_seq = 13; /* end */
        return 0;
    }

    pAcquireSRWLockExclusive(&condvar_srwlock);
    condvar_seq = 9;
    ret = pSleepConditionVariableSRW(cv, &condvar_srwlock, 200, 0);
    pReleaseSRWLockExclusive(&condvar_srwlock);
    ok (ret, "pSleepConditionVariableSRW should return TRUE on good wake\n");

    pAcquireSRWLockShared(&condvar_srwlock);
    condvar_seq = 10;
    ret = pSleepConditionVariableSRW(cv, &condvar_srwlock, 200, CONDITION_VARIABLE_LOCKMODE_SHARED);
    pReleaseSRWLockShared(&condvar_srwlock);
    ok (ret, "pSleepConditionVariableSRW should return TRUE on good wake\n");

    pAcquireSRWLockExclusive(&condvar_srwlock);
    condvar_seq = 11;
    ret = pSleepConditionVariableSRW(cv, &condvar_srwlock, 200, 0);
    pReleaseSRWLockExclusive(&condvar_srwlock);
    ok (ret, "pSleepConditionVariableSRW should return TRUE on good wake\n");

    pAcquireSRWLockShared(&condvar_srwlock);
    condvar_seq = 12;
    ret = pSleepConditionVariableSRW(cv, &condvar_srwlock, 200, CONDITION_VARIABLE_LOCKMODE_SHARED);
    pReleaseSRWLockShared(&condvar_srwlock);
    ok (ret, "pSleepConditionVariableSRW should return TRUE on good wake\n");

    condvar_seq = 13;
    return 0;
}

static void test_condvars_base(RTL_CONDITION_VARIABLE *cv)
{
    HANDLE hp, hc;
    DWORD dummy;
    BOOL ret;

    if (!pInitializeConditionVariable) {
        /* function is not yet in XP, only in newer Windows */
        win_skip("no condition variable support.\n");
        return;
    }

    InitializeCriticalSection (&condvar_crit);

    if (pInitializeSRWLock)
        pInitializeSRWLock(&condvar_srwlock);

    EnterCriticalSection (&condvar_crit);
    ret = pSleepConditionVariableCS(cv, &condvar_crit, 10);
    LeaveCriticalSection (&condvar_crit);

    ok (!ret, "SleepConditionVariableCS should return FALSE on untriggered condvar\n");
    ok (GetLastError() == ERROR_TIMEOUT, "SleepConditionVariableCS should return ERROR_TIMEOUT on untriggered condvar, not %ld\n", GetLastError());

    if (pInitializeSRWLock)
    {
        pAcquireSRWLockExclusive(&condvar_srwlock);
        ret = pSleepConditionVariableSRW(cv, &condvar_srwlock, 10, 0);
        pReleaseSRWLockExclusive(&condvar_srwlock);

        ok(!ret, "SleepConditionVariableSRW should return FALSE on untriggered condvar\n");
        ok(GetLastError() == ERROR_TIMEOUT, "SleepConditionVariableSRW should return ERROR_TIMEOUT on untriggered condvar, not %ld\n", GetLastError());

        pAcquireSRWLockShared(&condvar_srwlock);
        ret = pSleepConditionVariableSRW(cv, &condvar_srwlock, 10, CONDITION_VARIABLE_LOCKMODE_SHARED);
        pReleaseSRWLockShared(&condvar_srwlock);

        ok(!ret, "SleepConditionVariableSRW should return FALSE on untriggered condvar\n");
        ok(GetLastError() == ERROR_TIMEOUT, "SleepConditionVariableSRW should return ERROR_TIMEOUT on untriggered condvar, not %ld\n", GetLastError());
    }

    condvar_seq = 0;
    hp = CreateThread(NULL, 0, condvar_base_producer, cv, 0, &dummy);
    hc = CreateThread(NULL, 0, condvar_base_consumer, cv, 0, &dummy);

    condvar_seq = 1; /* go */

    while (condvar_seq < 9)
        Sleep (5);
    WaitForSingleObject(hp, 100);
    WaitForSingleObject(hc, 100);
}

static LONG srwlock_seq = 0;
static SRWLOCK aligned_srwlock;
static struct
{
    LONG wrong_execution_order;
    LONG samethread_excl_excl;
    LONG samethread_excl_shared;
    LONG samethread_shared_excl;
    LONG multithread_excl_excl;
    LONG excl_not_preferred;
    LONG trylock_excl;
    LONG trylock_shared;
} srwlock_base_errors;

#if defined(__i386__) || defined(__x86_64__)
#pragma pack(push,1)
struct
{
    char c;
    SRWLOCK lock;
} unaligned_srwlock;
#pragma pack(pop)
#endif

/* Sequence of acquire/release to check boundary conditions:
 *  0: init
 *
 *  1: thread2 acquires an exclusive lock and tries to acquire a second exclusive lock
 *  2: thread1 expects a deadlock and releases the waiting lock
 *     thread2 releases the lock again
 *
 *  3: thread2 acquires an exclusive lock and tries to acquire a shared lock
 *  4: thread1 expects a deadlock and releases the waiting lock
 *     thread2 releases the lock again
 *
 *  5: thread2 acquires a shared lock and tries to acquire an exclusive lock
 *  6: thread1 expects a deadlock and releases the waiting lock
 *     thread2 releases the lock again
 *
 *  7: thread2 acquires and releases two nested shared locks
 *
 *  8: thread1 acquires an exclusive lock
 *  9: thread2 tries to acquire the exclusive lock, too
 *     thread1 releases the exclusive lock again
 * 10: thread2 enters the exclusive lock and leaves it immediately again
 *
 * 11: thread1 acquires a shared lock
 * 12: thread2 acquires and releases a shared lock
 *     thread1 releases the lock again
 *
 * 13: thread1 acquires a shared lock
 * 14: thread2 tries to acquire an exclusive lock
 * 15: thread3 tries to acquire a shared lock
 * 16: thread1 releases the shared lock
 * 17: thread2 wakes up and releases the exclusive lock
 * 18: thread3 wakes up and releases the shared lock
 *
 * the following tests will only be executed if TryAcquireSRWLock* is available
 *
 * 19: thread1 calls TryAcquireSRWLockExclusive which should return TRUE
 *     thread1 checks the result of recursive calls to TryAcquireSRWLock*
 *     thread1 releases the exclusive lock
 *
 *     thread1 calls TryAcquireSRWLockShared which should return TRUE
 *     thread1 checks the result of recursive calls to TryAcquireSRWLock*
 *     thread1 releases the shared lock
 *
 *     thread1 acquires an exclusive lock
 * 20: thread2 calls TryAcquireSRWLockShared which should return FALSE
 *     thread2 calls TryAcquireSRWLockExclusive which should return FALSE
 * 21: thread1 releases the exclusive lock
 *
 *     thread1 acquires an shared lock
 * 22: thread2 calls TryAcquireSRWLockShared which should return TRUE
 *     thread2 calls TryAcquireSRWLockExclusive which should return FALSE
 * 23: thread1 releases the shared lock
 *
 *     thread1 acquires a shared lock and tries to acquire an exclusive lock
 * 24: thread2 calls TryAcquireSRWLockShared which should return FALSE
 *     thread2 calls TryAcquireSRWLockExclusive which should return FALSE
 * 25: thread1 releases the exclusive lock
 *
 *     thread1 acquires two shared locks
 * 26: thread2 calls TryAcquireSRWLockShared which should return TRUE
 *     thread2 calls TryAcquireSRWLockExclusive which should return FALSE
 * 27: thread1 releases one shared lock
 * 28: thread2 calls TryAcquireSRWLockShared which should return TRUE
 *     thread2 calls TryAcquireSRWLockExclusive which should return FALSE
 * 29: thread1 releases the second shared lock
 * 30: thread2 calls TryAcquireSRWLockShared which should return TRUE
 *     thread2 calls TryAcquireSRWLockExclusive which should return TRUE
 *
 * 31: end
 */

static DWORD WINAPI srwlock_base_thread1(void *arg)
{
    SRWLOCK *lock = arg;

    /* seq 2 */
    while (srwlock_seq < 2) Sleep(1);
    Sleep(100);
    if (InterlockedIncrement(&srwlock_seq) != 3)
        InterlockedIncrement(&srwlock_base_errors.samethread_excl_excl);
    pReleaseSRWLockExclusive(lock);

    /* seq 4 */
    while (srwlock_seq < 4) Sleep(1);
    Sleep(100);
    if (InterlockedIncrement(&srwlock_seq) != 5)
        InterlockedIncrement(&srwlock_base_errors.samethread_excl_shared);
    pReleaseSRWLockExclusive(lock);

    /* seq 6 */
    while (srwlock_seq < 6) Sleep(1);
    Sleep(100);
    if (InterlockedIncrement(&srwlock_seq) != 7)
        InterlockedIncrement(&srwlock_base_errors.samethread_shared_excl);
    pReleaseSRWLockShared(lock);

    /* seq 8 */
    while (srwlock_seq < 8) Sleep(1);
    pAcquireSRWLockExclusive(lock);
    if (InterlockedIncrement(&srwlock_seq) != 9)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);
    Sleep(100);
    if (InterlockedIncrement(&srwlock_seq) != 10)
        InterlockedIncrement(&srwlock_base_errors.multithread_excl_excl);
    pReleaseSRWLockExclusive(lock);

    /* seq 11 */
    while (srwlock_seq < 11) Sleep(1);
    pAcquireSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 12)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 13 */
    while (srwlock_seq < 13) Sleep(1);
    pReleaseSRWLockShared(lock);
    pAcquireSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 14)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 16 */
    while (srwlock_seq < 16) Sleep(1);
    Sleep(50); /* ensure that both the exclusive and shared access thread are queued */
    if (InterlockedIncrement(&srwlock_seq) != 17)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);
    pReleaseSRWLockShared(lock);

    /* skip over remaining tests if TryAcquireSRWLock* is not available */
    if (!pTryAcquireSRWLockExclusive)
        return 0;

    /* seq 19 */
    while (srwlock_seq < 19) Sleep(1);
    if (pTryAcquireSRWLockExclusive(lock))
    {
        if (pTryAcquireSRWLockShared(lock))
            InterlockedIncrement(&srwlock_base_errors.trylock_shared);
        if (pTryAcquireSRWLockExclusive(lock))
            InterlockedIncrement(&srwlock_base_errors.trylock_excl);
        pReleaseSRWLockExclusive(lock);
    }
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);

    if (pTryAcquireSRWLockShared(lock))
    {
        if (pTryAcquireSRWLockShared(lock))
            pReleaseSRWLockShared(lock);
        else
            InterlockedIncrement(&srwlock_base_errors.trylock_shared);
        if (pTryAcquireSRWLockExclusive(lock))
            InterlockedIncrement(&srwlock_base_errors.trylock_excl);
        pReleaseSRWLockShared(lock);
    }
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_shared);

    pAcquireSRWLockExclusive(lock);
    if (InterlockedIncrement(&srwlock_seq) != 20)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 21 */
    while (srwlock_seq < 21) Sleep(1);
    pReleaseSRWLockExclusive(lock);
    pAcquireSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 22)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 23 */
    while (srwlock_seq < 23) Sleep(1);
    pReleaseSRWLockShared(lock);
    pAcquireSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 24)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 25 */
    pAcquireSRWLockExclusive(lock);
    if (srwlock_seq != 25)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);
    pReleaseSRWLockExclusive(lock);

    pAcquireSRWLockShared(lock);
    pAcquireSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 26)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 27 */
    while (srwlock_seq < 27) Sleep(1);
    pReleaseSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 28)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 29 */
    while (srwlock_seq < 29) Sleep(1);
    pReleaseSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 30)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    return 0;
}

static DWORD WINAPI srwlock_base_thread2(void *arg)
{
    SRWLOCK *lock = arg;

    /* seq 1 */
    while (srwlock_seq < 1) Sleep(1);
    pAcquireSRWLockExclusive(lock);
    if (InterlockedIncrement(&srwlock_seq) != 2)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 3 */
    pAcquireSRWLockExclusive(lock);
    if (srwlock_seq != 3)
        InterlockedIncrement(&srwlock_base_errors.samethread_excl_excl);
    pReleaseSRWLockExclusive(lock);
    pAcquireSRWLockExclusive(lock);
    if (InterlockedIncrement(&srwlock_seq) != 4)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 5 */
    pAcquireSRWLockShared(lock);
    if (srwlock_seq != 5)
        InterlockedIncrement(&srwlock_base_errors.samethread_excl_shared);
    pReleaseSRWLockShared(lock);
    pAcquireSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 6)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 7 */
    pAcquireSRWLockExclusive(lock);
    if (srwlock_seq != 7)
        InterlockedIncrement(&srwlock_base_errors.samethread_shared_excl);
    pReleaseSRWLockExclusive(lock);
    pAcquireSRWLockShared(lock);
    pAcquireSRWLockShared(lock);
    pReleaseSRWLockShared(lock);
    pReleaseSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 8)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 9, 10 */
    while (srwlock_seq < 9) Sleep(1);
    pAcquireSRWLockExclusive(lock);
    if (srwlock_seq != 10)
        InterlockedIncrement(&srwlock_base_errors.multithread_excl_excl);
    pReleaseSRWLockExclusive(lock);
    if (InterlockedIncrement(&srwlock_seq) != 11)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 12 */
    while (srwlock_seq < 12) Sleep(1);
    pAcquireSRWLockShared(lock);
    pReleaseSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 13)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 14 */
    while (srwlock_seq < 14) Sleep(1);
    if (InterlockedIncrement(&srwlock_seq) != 15)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 17 */
    pAcquireSRWLockExclusive(lock);
    if (srwlock_seq != 17)
        InterlockedIncrement(&srwlock_base_errors.excl_not_preferred);
    if (InterlockedIncrement(&srwlock_seq) != 18)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);
    pReleaseSRWLockExclusive(lock);

    /* skip over remaining tests if TryAcquireSRWLock* is not available */
    if (!pTryAcquireSRWLockExclusive)
        return 0;

    /* seq 20 */
    while (srwlock_seq < 20) Sleep(1);
    if (pTryAcquireSRWLockShared(lock))
        InterlockedIncrement(&srwlock_base_errors.trylock_shared);
    if (pTryAcquireSRWLockExclusive(lock))
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);
    if (InterlockedIncrement(&srwlock_seq) != 21)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 22 */
    while (srwlock_seq < 22) Sleep(1);
    if (pTryAcquireSRWLockShared(lock))
        pReleaseSRWLockShared(lock);
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_shared);
    if (pTryAcquireSRWLockExclusive(lock))
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);
    if (InterlockedIncrement(&srwlock_seq) != 23)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 24 */
    while (srwlock_seq < 24) Sleep(1);
    Sleep(50); /* ensure that exclusive access request is queued */
    if (pTryAcquireSRWLockShared(lock))
    {
        pReleaseSRWLockShared(lock);
        InterlockedIncrement(&srwlock_base_errors.excl_not_preferred);
    }
    if (pTryAcquireSRWLockExclusive(lock))
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);
    if (InterlockedIncrement(&srwlock_seq) != 25)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);
    pReleaseSRWLockShared(lock);

    /* seq 26 */
    while (srwlock_seq < 26) Sleep(1);
    if (pTryAcquireSRWLockShared(lock))
        pReleaseSRWLockShared(lock);
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_shared);
    if (pTryAcquireSRWLockExclusive(lock))
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);
    if (InterlockedIncrement(&srwlock_seq) != 27)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 28 */
    while (srwlock_seq < 28) Sleep(1);
    if (pTryAcquireSRWLockShared(lock))
        pReleaseSRWLockShared(lock);
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_shared);
    if (pTryAcquireSRWLockExclusive(lock))
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);
    if (InterlockedIncrement(&srwlock_seq) != 29)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 30 */
    while (srwlock_seq < 30) Sleep(1);
    if (pTryAcquireSRWLockShared(lock))
        pReleaseSRWLockShared(lock);
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_shared);
    if (pTryAcquireSRWLockExclusive(lock))
        pReleaseSRWLockExclusive(lock);
    else
        InterlockedIncrement(&srwlock_base_errors.trylock_excl);
    if (InterlockedIncrement(&srwlock_seq) != 31)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    return 0;
}

static DWORD WINAPI srwlock_base_thread3(void *arg)
{
    SRWLOCK *lock = arg;

    /* seq 15 */
    while (srwlock_seq < 15) Sleep(1);
    Sleep(50); /* some delay, so that thread2 can try to acquire a second exclusive lock */
    if (InterlockedIncrement(&srwlock_seq) != 16)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* seq 18 */
    pAcquireSRWLockShared(lock);
    if (srwlock_seq != 18)
        InterlockedIncrement(&srwlock_base_errors.excl_not_preferred);
    pReleaseSRWLockShared(lock);
    if (InterlockedIncrement(&srwlock_seq) != 19)
        InterlockedIncrement(&srwlock_base_errors.wrong_execution_order);

    /* skip over remaining tests if TryAcquireSRWLock* is not available */
    if (!pTryAcquireSRWLockExclusive)
    {
        /* function is only in Windows 7 and newer */
        win_skip("no srw trylock support.\n");
        srwlock_seq = 31; /* end */
        return 0;
    }

    return 0;
}

static void test_srwlock_base(SRWLOCK *lock)
{
    HANDLE h1, h2, h3;
    DWORD dummy;

    if (!pInitializeSRWLock)
    {
        /* function is not yet in XP, only in newer Windows */
        win_skip("no srw lock support.\n");
        return;
    }

    pInitializeSRWLock(lock);
    memset(&srwlock_base_errors, 0, sizeof(srwlock_base_errors));
    srwlock_seq = 0;

    h1 = CreateThread(NULL, 0, srwlock_base_thread1, lock, 0, &dummy);
    h2 = CreateThread(NULL, 0, srwlock_base_thread2, lock, 0, &dummy);
    h3 = CreateThread(NULL, 0, srwlock_base_thread3, lock, 0, &dummy);

    srwlock_seq = 1; /* go */
    while (srwlock_seq < 31)
        Sleep(5);

    WaitForSingleObject(h1, 100);
    WaitForSingleObject(h2, 100);
    WaitForSingleObject(h3, 100);

    ok(!srwlock_base_errors.wrong_execution_order,
            "thread commands were executed in the wrong order (occurred %ld times).\n",
            srwlock_base_errors.wrong_execution_order);

    ok(!srwlock_base_errors.samethread_excl_excl,
            "AcquireSRWLockExclusive didn't block when called multiple times from the same thread (occurred %ld times).\n",
            srwlock_base_errors.samethread_excl_excl);

    ok(!srwlock_base_errors.samethread_excl_shared,
            "AcquireSRWLockShared didn't block when the same thread holds an exclusive lock (occurred %ld times).\n",
            srwlock_base_errors.samethread_excl_shared);

    ok(!srwlock_base_errors.samethread_shared_excl,
            "AcquireSRWLockExclusive didn't block when the same thread holds a shared lock (occurred %ld times).\n",
            srwlock_base_errors.samethread_shared_excl);

    ok(!srwlock_base_errors.multithread_excl_excl,
            "AcquireSRWLockExclusive didn't block when a second thread holds the exclusive lock (occurred %ld times).\n",
            srwlock_base_errors.multithread_excl_excl);

    ok(!srwlock_base_errors.excl_not_preferred,
            "thread waiting for exclusive access to the SHMLock was not preferred (occurred %ld times).\n",
            srwlock_base_errors.excl_not_preferred);

    ok(!srwlock_base_errors.trylock_excl,
            "TryAcquireSRWLockExclusive didn't behave as expected (occurred %ld times).\n",
            srwlock_base_errors.trylock_excl);

    ok(!srwlock_base_errors.trylock_shared,
            "TryAcquireSRWLockShared didn't behave as expected (occurred %ld times).\n",
            srwlock_base_errors.trylock_shared);

}

static SRWLOCK srwlock_example;
static LONG srwlock_protected_value = 0;
static LONG srwlock_example_errors = 0, srwlock_inside = 0, srwlock_cnt = 0;
static BOOL srwlock_stop = FALSE;

static DWORD WINAPI srwlock_example_thread(LPVOID x) {
    DWORD *cnt = x;
    LONG old;

    while (!srwlock_stop)
    {

        /* periodically request exclusive access */
        if (InterlockedIncrement(&srwlock_cnt) % 13 == 0)
        {
            pAcquireSRWLockExclusive(&srwlock_example);
            if (InterlockedIncrement(&srwlock_inside) != 1)
                InterlockedIncrement(&srwlock_example_errors);

            InterlockedIncrement(&srwlock_protected_value);
            Sleep(1);

            if (InterlockedDecrement(&srwlock_inside) != 0)
                InterlockedIncrement(&srwlock_example_errors);
            pReleaseSRWLockExclusive(&srwlock_example);
        }

        /* request shared access */
        pAcquireSRWLockShared(&srwlock_example);
        InterlockedIncrement(&srwlock_inside);
        old = srwlock_protected_value;

        (*cnt)++;
        Sleep(1);

        if (old != srwlock_protected_value)
            InterlockedIncrement(&srwlock_example_errors);
        InterlockedDecrement(&srwlock_inside);
        pReleaseSRWLockShared(&srwlock_example);
    }

    return 0;
}

static void test_srwlock_example(void)
{
    HANDLE h1, h2, h3;
    DWORD dummy;
    DWORD cnt1, cnt2, cnt3;

    if (!pInitializeSRWLock) {
        /* function is not yet in XP, only in newer Windows */
        win_skip("no srw lock support.\n");
        return;
    }

    pInitializeSRWLock(&srwlock_example);

    cnt1 = cnt2 = cnt3 = 0;

    h1 = CreateThread(NULL, 0, srwlock_example_thread, &cnt1, 0, &dummy);
    h2 = CreateThread(NULL, 0, srwlock_example_thread, &cnt2, 0, &dummy);
    h3 = CreateThread(NULL, 0, srwlock_example_thread, &cnt3, 0, &dummy);

    /* limit run to 1 second. */
    Sleep(1000);

    /* tear down start */
    srwlock_stop = TRUE;

    WaitForSingleObject(h1, 1000);
    WaitForSingleObject(h2, 1000);
    WaitForSingleObject(h3, 1000);

    ok(!srwlock_inside, "threads didn't terminate properly, srwlock_inside is %ld.\n", srwlock_inside);
    ok(!srwlock_example_errors, "errors occurred while running SRWLock example test (number of errors: %ld)\n",
            srwlock_example_errors);

    trace("number of shared accesses per thread are c1 %ld, c2 %ld, c3 %ld\n", cnt1, cnt2, cnt3);
    trace("number of total exclusive accesses is %ld\n", srwlock_protected_value);
}

static void test_srwlock_quirk(void)
{
    union { SRWLOCK *s; LONG *l; } u = { &srwlock_example };

    if (!pInitializeSRWLock) {
        /* function is not yet in XP, only in newer Windows */
        win_skip("no srw lock support.\n");
        return;
    }

    /* WeCom 4.x checks releasing a lock with value 0x1 results in it becoming 0x0. */
    *u.l = 1;
    pReleaseSRWLockExclusive(&srwlock_example);
    ok(*u.l == 0, "expected 0x0, got %lx\n", *u.l);
}

static DWORD WINAPI alertable_wait_thread(void *param)
{
    HANDLE *semaphores = param;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    DWORD result;

    ReleaseSemaphore(semaphores[0], 1, NULL);
    result = WaitForMultipleObjectsEx(1, &semaphores[1], TRUE, 1000, TRUE);
    ok(result == WAIT_IO_COMPLETION, "expected WAIT_IO_COMPLETION, got %lu\n", result);
    result = WaitForMultipleObjectsEx(1, &semaphores[1], TRUE, 200, TRUE);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);

    ReleaseSemaphore(semaphores[0], 1, NULL);
    timeout.QuadPart = -10000000;
    status = pNtWaitForMultipleObjects(1, &semaphores[1], FALSE, TRUE, &timeout);
    ok(status == STATUS_USER_APC, "expected STATUS_USER_APC, got %08lx\n", status);
    timeout.QuadPart = -2000000;
    status = pNtWaitForMultipleObjects(1, &semaphores[1], FALSE, TRUE, &timeout);
    ok(status == STATUS_WAIT_0, "expected STATUS_WAIT_0, got %08lx\n", status);

    ReleaseSemaphore(semaphores[0], 1, NULL);
    timeout.QuadPart = -10000000;
    status = pNtWaitForMultipleObjects(1, &semaphores[1], FALSE, TRUE, &timeout);
    ok(status == STATUS_USER_APC, "expected STATUS_USER_APC, got %08lx\n", status);
    result = WaitForSingleObject(semaphores[0], 0);
    ok(result == WAIT_TIMEOUT, "expected WAIT_TIMEOUT, got %lu\n", result);

    return 0;
}

static void CALLBACK alertable_wait_apc(ULONG_PTR userdata)
{
    HANDLE *semaphores = (void *)userdata;
    ReleaseSemaphore(semaphores[1], 1, NULL);
}

static void CALLBACK alertable_wait_apc2(ULONG_PTR userdata)
{
    HANDLE *semaphores = (void *)userdata;
    DWORD result;

    result = WaitForSingleObject(semaphores[0], 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
}

static void test_alertable_wait(void)
{
    HANDLE thread, semaphores[2];
    DWORD result;

    semaphores[0] = CreateSemaphoreW(NULL, 0, 2, NULL);
    ok(semaphores[0] != NULL, "CreateSemaphore failed with %lu\n", GetLastError());
    semaphores[1] = CreateSemaphoreW(NULL, 0, 1, NULL);
    ok(semaphores[1] != NULL, "CreateSemaphore failed with %lu\n", GetLastError());
    thread = CreateThread(NULL, 0, alertable_wait_thread, semaphores, 0, NULL);
    ok(thread != NULL, "CreateThread failed with %lu\n", GetLastError());

    result = WaitForSingleObject(semaphores[0], 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
    Sleep(100); /* ensure the thread is blocking in WaitForMultipleObjectsEx */
    result = QueueUserAPC(alertable_wait_apc, thread, (ULONG_PTR)semaphores);
    ok(result != 0, "QueueUserAPC failed with %lu\n", GetLastError());

    result = WaitForSingleObject(semaphores[0], 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
    Sleep(100); /* ensure the thread is blocking in NtWaitForMultipleObjects */
    result = QueueUserAPC(alertable_wait_apc, thread, (ULONG_PTR)semaphores);
    ok(result != 0, "QueueUserAPC failed with %lu\n", GetLastError());

    result = WaitForSingleObject(semaphores[0], 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
    Sleep(100); /* ensure the thread is blocking in NtWaitForMultipleObjects */
    result = QueueUserAPC(alertable_wait_apc2, thread, (ULONG_PTR)semaphores);
    ok(result != 0, "QueueUserAPC failed with %lu\n", GetLastError());
    result = QueueUserAPC(alertable_wait_apc2, thread, (ULONG_PTR)semaphores);
    ok(result != 0, "QueueUserAPC failed with %lu\n", GetLastError());
    ReleaseSemaphore(semaphores[0], 2, NULL);

    result = WaitForSingleObject(thread, 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
    CloseHandle(thread);
    CloseHandle(semaphores[0]);
    CloseHandle(semaphores[1]);
}

struct apc_deadlock_info
{
    PROCESS_INFORMATION *pi;
    HANDLE event;
    BOOL running;
};

static DWORD WINAPI apc_deadlock_thread(void *param)
{
    struct apc_deadlock_info *info = param;
    PROCESS_INFORMATION *pi = info->pi;
    NTSTATUS status;
    SIZE_T size;
    void *base;

    while (info->running)
    {
        base = NULL;
        size = 0x1000;
        status = pNtAllocateVirtualMemory(pi->hProcess, &base, 0, &size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        ok(!status, "expected STATUS_SUCCESS, got %08lx\n", status);
        ok(base != NULL, "expected base != NULL, got %p\n", base);
        SetEvent(info->event);

        size = 0;
        status = pNtFreeVirtualMemory(pi->hProcess, &base, &size, MEM_RELEASE);
        ok(!status, "expected STATUS_SUCCESS, got %08lx\n", status);
        SetEvent(info->event);
    }

    return 0;
}

static void test_apc_deadlock(void)
{
    struct apc_deadlock_info info;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si = { sizeof(si) };
    char cmdline[MAX_PATH];
    HANDLE event, thread;
    DWORD result;
    BOOL success;
    char **argv;
    int i;

    winetest_get_mainargs(&argv);
    sprintf(cmdline, "\"%s\" sync apc_deadlock", argv[0]);
    success = CreateProcessA(argv[0], cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    ok(success, "CreateProcess failed with %lu\n", GetLastError());

    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    ok(event != NULL, "CreateEvent failed with %lu\n", GetLastError());

    info.pi = &pi;
    info.event = event;
    info.running = TRUE;

    thread = CreateThread(NULL, 0, apc_deadlock_thread, &info, 0, NULL);
    ok(thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    result = WaitForSingleObject(event, 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);

    for (i = 0; i < 1000 && info.running; i++)
    {
        result = SuspendThread(pi.hThread);
        ok(result == 0, "expected 0, got %lu\n", result);

        WaitForSingleObject(event, 0); /* reset event */
        result = WaitForSingleObject(event, 1000);
        if (result == WAIT_TIMEOUT)
        {
            todo_wine
            ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
            info.running = FALSE;
        }
        else
            ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);

        result = ResumeThread(pi.hThread);
        ok(result == 1, "expected 1, got %lu\n", result);
        Sleep(1);
    }

    info.running = FALSE;
    result = WaitForSingleObject(thread, 1000);
    ok(result == WAIT_OBJECT_0, "expected WAIT_OBJECT_0, got %lu\n", result);
    CloseHandle(thread);
    CloseHandle(event);

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static jmp_buf bad_cs_jmpbuf;

static LONG WINAPI bad_cs_handler( EXCEPTION_POINTERS *eptr )
{
    EXCEPTION_RECORD *rec = eptr->ExceptionRecord;

    ok(!rec->NumberParameters, "got %lu.\n", rec->NumberParameters);
    ok(rec->ExceptionFlags == EXCEPTION_NONCONTINUABLE
            || rec->ExceptionFlags == (EXCEPTION_NONCONTINUABLE | EXCEPTION_SOFTWARE_ORIGINATE),
            "got %#lx.\n", rec->ExceptionFlags);
    longjmp(bad_cs_jmpbuf, rec->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void test_crit_section(void)
{
    void *vectored_handler;
    CRITICAL_SECTION cs;
    int exc_code;
    HANDLE old;
    BOOL ret;

    /* Win8+ does not initialize debug info, one has to use RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO
       to override that. */
    memset(&cs, 0, sizeof(cs));
    InitializeCriticalSection(&cs);
    ok(cs.DebugInfo == (void *)(ULONG_PTR)-1 || broken(!!cs.DebugInfo) /* before Win8 */,
            "Unexpected debug info pointer %p.\n", cs.DebugInfo);
    DeleteCriticalSection(&cs);
    ok(cs.DebugInfo == NULL, "Unexpected debug info pointer %p.\n", cs.DebugInfo);

    if (!pInitializeCriticalSectionEx)
    {
        win_skip("InitializeCriticalSectionEx isn't available, skipping tests.\n");
        return;
    }

    memset(&cs, 0, sizeof(cs));
    ret = pInitializeCriticalSectionEx(&cs, 0, 0);
    ok(ret, "Failed to initialize critical section.\n");
    ok(cs.DebugInfo == (void *)(ULONG_PTR)-1  || broken(!!cs.DebugInfo) /* before Win8 */,
            "Unexpected debug info pointer %p.\n", cs.DebugInfo);
    DeleteCriticalSection(&cs);
    ok(cs.DebugInfo == NULL, "Unexpected debug info pointer %p.\n", cs.DebugInfo);

    memset(&cs, 0, sizeof(cs));
    ret = pInitializeCriticalSectionEx(&cs, 0, CRITICAL_SECTION_NO_DEBUG_INFO);
    ok(ret, "Failed to initialize critical section.\n");
    ok(cs.DebugInfo == (void *)(ULONG_PTR)-1, "Unexpected debug info pointer %p.\n", cs.DebugInfo);
    DeleteCriticalSection(&cs);
    ok(cs.DebugInfo == NULL, "Unexpected debug info pointer %p.\n", cs.DebugInfo);

    memset(&cs, 0, sizeof(cs));
    ret = pInitializeCriticalSectionEx(&cs, 0, 0);
    ok(ret, "Failed to initialize critical section.\n");
    ok(cs.DebugInfo == (void *)(ULONG_PTR)-1 || broken(!!cs.DebugInfo) /* before Win8 */,
            "Unexpected debug info pointer %p.\n", cs.DebugInfo);
    DeleteCriticalSection(&cs);
    ok(cs.DebugInfo == NULL, "Unexpected debug info pointer %p.\n", cs.DebugInfo);

    memset(&cs, 0, sizeof(cs));
    ret = pInitializeCriticalSectionEx(&cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    ok(ret || broken(GetLastError() == ERROR_INVALID_PARAMETER) /* before Win8 */,
            "Failed to initialize critical section, error %lu.\n", GetLastError());
    if (!ret)
    {
        ret = pInitializeCriticalSectionEx(&cs, 0, 0);
        ok(ret, "Failed to initialize critical section.\n");
    }
    ok(cs.DebugInfo && cs.DebugInfo != (void *)(ULONG_PTR)-1, "Unexpected debug info pointer %p.\n", cs.DebugInfo);

    ret = TryEnterCriticalSection(&cs);
    ok(ret, "Failed to enter critical section.\n");
    LeaveCriticalSection(&cs);

    cs.DebugInfo = NULL;

    ret = TryEnterCriticalSection(&cs);
    ok(ret, "Failed to enter critical section.\n");
    LeaveCriticalSection(&cs);

    DeleteCriticalSection(&cs);
    ok(cs.DebugInfo == NULL, "Unexpected debug info pointer %p.\n", cs.DebugInfo);

    ret = pInitializeCriticalSectionEx(&cs, 0, 0);
    ok(ret, "got error %lu.\n", GetLastError());
    old = cs.LockSemaphore;
    cs.LockSemaphore = (HANDLE)0xdeadbeef;

    cs.LockCount = 0;
    vectored_handler = AddVectoredExceptionHandler(TRUE, bad_cs_handler);
    if (!(exc_code = setjmp(bad_cs_jmpbuf)))
        EnterCriticalSection(&cs);
    ok(cs.LockCount, "got %ld.\n", cs.LockCount);
    ok(exc_code == STATUS_INVALID_HANDLE, "got %#x.\n", exc_code);
    RemoveVectoredExceptionHandler(vectored_handler);
    cs.LockSemaphore = old;
    DeleteCriticalSection(&cs);
}

static DWORD WINAPI thread_proc(LPVOID unused)
{
    Sleep(INFINITE);
    return 0;
}

static int apc_count;

static void CALLBACK user_apc(ULONG_PTR unused)
{
    apc_count++;
}

static void CALLBACK call_user_apc(ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3)
{
    PAPCFUNC func = (PAPCFUNC)arg1;
    func(arg2);
}

static void test_QueueUserAPC(void)
{
    HANDLE thread;
    DWORD tid, ret;
    NTSTATUS status;

    thread = CreateThread(NULL, 0, thread_proc, NULL, CREATE_SUSPENDED, &tid);
    ok(thread != NULL, "CreateThread error %lu\n", GetLastError());

    ret = TerminateThread(thread, 0xdeadbeef);
    ok(ret, "TerminateThread error %lu\n", GetLastError());

    ret = WaitForSingleObject(thread, 1000);
    ok(ret == WAIT_OBJECT_0, "got %lu\n", ret);

    ret = pNtQueueApcThread(thread, call_user_apc, (ULONG_PTR)user_apc, 0, 0);
    ok(ret == STATUS_UNSUCCESSFUL, "got %#lx\n", ret);
    ret = pNtQueueApcThread(thread, NULL, 0, 0, 0);
    ok(ret == STATUS_UNSUCCESSFUL, "got %#lx\n", ret);

    SetLastError(0xdeadbeef);
    ret = QueueUserAPC(user_apc, thread, 0);
    ok(!ret, "QueueUserAPC should fail\n");
    ok(GetLastError() == ERROR_GEN_FAILURE, "got %lu\n", GetLastError());

    CloseHandle(thread);

    apc_count = 0;
    ret = QueueUserAPC(user_apc, GetCurrentThread(), 0);
    ok(ret, "QueueUserAPC failed err %lu\n", GetLastError());
    ok(!apc_count, "APC count %u\n", apc_count);
    ret = SleepEx( 100, TRUE );
    ok( ret == WAIT_IO_COMPLETION, "SleepEx returned %lu\n", ret);
    ok(apc_count == 1, "APC count %u\n", apc_count);

    ret = pNtQueueApcThread( GetCurrentThread(), NULL, 0, 0, 0 );
    ok( !ret, "got %#lx\n", ret);
    ret = SleepEx( 100, TRUE );
    ok( ret == WAIT_OBJECT_0, "SleepEx returned %lu\n", ret);

    apc_count = 0;
    ret = QueueUserAPC(user_apc, GetCurrentThread(), 0);
    ok(ret, "QueueUserAPC failed err %lu\n", GetLastError());
    ok(!apc_count, "APC count %u\n", apc_count);
    status = pNtTestAlert();
    ok(!status, "got %lx\n", status);
    ok(apc_count == 1, "APC count %u\n", apc_count);
    status = pNtTestAlert();
    ok(!status, "got %lx\n", status);
    ok(apc_count == 1, "APC count %u\n", apc_count);
}

START_TEST(sync)
{
    char **argv;
    int argc;
    HMODULE hdll = GetModuleHandleA("kernel32.dll");
    HMODULE hntdll = GetModuleHandleA("ntdll.dll");

    pInitOnceInitialize = (void *)GetProcAddress(hdll, "InitOnceInitialize");
    pInitOnceExecuteOnce = (void *)GetProcAddress(hdll, "InitOnceExecuteOnce");
    pInitOnceBeginInitialize = (void *)GetProcAddress(hdll, "InitOnceBeginInitialize");
    pInitOnceComplete = (void *)GetProcAddress(hdll, "InitOnceComplete");
    pInitializeConditionVariable = (void *)GetProcAddress(hdll, "InitializeConditionVariable");
    pSleepConditionVariableCS = (void *)GetProcAddress(hdll, "SleepConditionVariableCS");
    pSleepConditionVariableSRW = (void *)GetProcAddress(hdll, "SleepConditionVariableSRW");
    pWakeAllConditionVariable = (void *)GetProcAddress(hdll, "WakeAllConditionVariable");
    pWakeConditionVariable = (void *)GetProcAddress(hdll, "WakeConditionVariable");
    pInitializeCriticalSectionEx = (void *)GetProcAddress(hdll, "InitializeCriticalSectionEx");
    pInitializeSRWLock = (void *)GetProcAddress(hdll, "InitializeSRWLock");
    pAcquireSRWLockExclusive = (void *)GetProcAddress(hdll, "AcquireSRWLockExclusive");
    pAcquireSRWLockShared = (void *)GetProcAddress(hdll, "AcquireSRWLockShared");
    pReleaseSRWLockExclusive = (void *)GetProcAddress(hdll, "ReleaseSRWLockExclusive");
    pReleaseSRWLockShared = (void *)GetProcAddress(hdll, "ReleaseSRWLockShared");
    pTryAcquireSRWLockExclusive = (void *)GetProcAddress(hdll, "TryAcquireSRWLockExclusive");
    pTryAcquireSRWLockShared = (void *)GetProcAddress(hdll, "TryAcquireSRWLockShared");
    pNtAllocateVirtualMemory = (void *)GetProcAddress(hntdll, "NtAllocateVirtualMemory");
    pNtFreeVirtualMemory = (void *)GetProcAddress(hntdll, "NtFreeVirtualMemory");
    pNtWaitForSingleObject = (void *)GetProcAddress(hntdll, "NtWaitForSingleObject");
    pNtWaitForMultipleObjects = (void *)GetProcAddress(hntdll, "NtWaitForMultipleObjects");
    pRtlInterlockedPushListSList = (void *)GetProcAddress(hntdll, "RtlInterlockedPushListSList");
    pRtlInterlockedPushListSListEx = (void *)GetProcAddress(hntdll, "RtlInterlockedPushListSListEx");
    pNtQueueApcThread = (void *)GetProcAddress(hntdll, "NtQueueApcThread");
    pNtTestAlert = (void *)GetProcAddress(hntdll, "NtTestAlert");

    argc = winetest_get_mainargs( &argv );
    if (argc >= 3)
    {
        if (!strcmp(argv[2], "apc_deadlock"))
        {
            for (;;) SleepEx(INFINITE, TRUE);
        }
        return;
    }

    init_fastcall_thunk();

    test_QueueUserAPC();
    test_signalandwait();
    test_temporary_objects();
    test_mutex();
    test_slist();
    test_event();
    test_semaphore();
    test_waitable_timer();
    test_iocp_callback();
    test_timer_queue();
    test_WaitForSingleObject();
    test_WaitForMultipleObjects();
    test_initonce();
    test_condvars_base(&aligned_cv);
    test_condvars_base(&unaligned_cv.cv);
    test_condvars_consumer_producer();
    test_srwlock_base(&aligned_srwlock);
    test_srwlock_quirk();
#if defined(__i386__) || defined(__x86_64__)
    /* unaligned locks only work on x86 platforms */
    test_srwlock_base(&unaligned_srwlock.lock);
#endif
    test_srwlock_example();
    test_alertable_wait();
    test_apc_deadlock();
    test_crit_section();
}
