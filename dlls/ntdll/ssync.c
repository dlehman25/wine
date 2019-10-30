/*
 * Shared Memory synchronization functions
 *
 * Copyright 2019 Daniel Lehman
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

#include "config.h"
#include "wine/port.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "ntdll_misc.h"

#include "wine/debug.h"
#include "wine/shmlib.h"
#include "wine/ssync.h"
#include "ssync.h"

WINE_DEFAULT_DEBUG_CHANNEL(ssync);

struct ss_cache_entry
{
    int                 lock;
    struct ss_obj_base *ptr;
};

struct ss_cache
{
    unsigned int nentries;
    struct ss_cache_entry entries[1];
};

struct ss_cache *ss_state;

static inline void * __WINE_ALLOC_SIZE(1) heap_alloc(SIZE_T len)
{
    return RtlAllocateHeap(GetProcessHeap(), 0, len);
}
static inline void * __WINE_ALLOC_SIZE(1) heap_alloc_zero(SIZE_T len)
{
    return RtlAllocateHeap(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}

static inline void heap_free(void *mem)
{
    RtlFreeHeap(GetProcessHeap(), 0, mem);
}

static inline unsigned int ss_handle_to_index(obj_handle_t handle)
{
    return (handle >> 2) - 1;
}

int ss_init(void)
{
    static const int MAX_ENTRIES = 4096;

    TRACE("\n");

    if (!(ss_state = heap_alloc_zero(offsetof(struct ss_cache, entries[MAX_ENTRIES]))))
        return -1;

    ss_state->nentries = MAX_ENTRIES;
    return 0;
}

int ss_term(void)
{
    FIXME("stub\n");
    return -1;
}

int ss_set_handle(obj_handle_t handle, shm_ptr_t shm_ptr)
{
    struct ss_obj_base *ss_obj;
    struct ss_obj_mutex *mutex;
    unsigned int idx;

    if (!(ss_obj = shm_ptr_to_void_ptr(shm_ptr)))
        return -1;

    mutex = &ss_obj->u.mutex;
    MESSAGE("%s: pid 0x%04x tid %d 0x%x -> 0x%08x -> %p (%p tid %u cnd %u abd %d)\n",
                __FUNCTION__, GetCurrentProcessId(), GetCurrentThreadId(),
                handle, shm_ptr, ss_obj, mutex, mutex->owner, mutex->count, mutex->abandoned);
    idx = ss_handle_to_index(handle);
    ss_state->entries[idx].ptr = ss_obj; /* TODO: old one? lock? */
    return 0;
}
