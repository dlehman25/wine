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

int ss_set_handle(obj_handle_t handle, shm_ptr_t shm_ptr)
{
    struct ss_obj_base *ss_obj;
    struct ss_obj_mutex *mutex;
    if (!(ss_obj = shm_ptr_to_void_ptr(shm_ptr)))
        return -1;

    mutex = &ss_obj->u.mutex;
    MESSAGE("%s: pid 0x%04x tid %d 0x%x -> 0x%08x -> %p (%p tid %u cnd %u abd %d)\n",
                __FUNCTION__, GetCurrentProcessId(), GetCurrentThreadId(),
                handle, shm_ptr, ss_obj, mutex, mutex->owner, mutex->count, mutex->abandoned);
    return 0;
}
