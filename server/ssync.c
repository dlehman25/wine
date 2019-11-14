/*
 * Server-side shared memory synchronization
 *
 * Copyright (C) 2019 Daniel Lehman
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

#include "wine/list.h"
#include "wine/shmlib.h"
#include "shmlib.h"
#include "ssync.h"

#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdio.h>      // TODO:

#ifdef __linux__
#define FUTEX_WAIT 0
static int futex_private = 128;

static inline int futex_wait( const int *addr, int val, struct timespec *timeout )
{
    return syscall( __NR_futex, addr, FUTEX_WAIT | futex_private, val, timeout, 0, 0 );
}

static inline int futexes_supported(void)
{
    static int supported = -1;

    if (supported == -1)
    {
        futex_wait( &supported, 10, NULL );
        if (errno == ENOSYS)
        {
            futex_private = 0;
            futex_wait( &supported, 10, NULL );
        }
        supported = (errno != ENOSYS);
    }
    return supported;
}
#else
static inline int futexes_supported(void) { return FALSE; }
#endif

static struct ss_obj *ss_alloc(void)
{
    shm_ptr_t shm_ptr;
    struct ss_obj *obj;

    if (!futexes_supported())
        return NULL;

    if ((shm_ptr = shm_malloc(sizeof(*obj))) == SHM_NULL)
        return NULL;

    obj = shm_ptr_to_void_ptr(shm_ptr);
    memset(obj, 0, sizeof(*obj));
    obj->shm_ptr = shm_ptr;
    return obj;
}

void ss_free(struct ss_obj *obj)
{
    if (!obj) return;
    shm_free(obj->shm_ptr);
}

struct ss_obj *ss_alloc_mutex(unsigned int owner)
{
    struct ss_obj *obj;

    if (!(obj = ss_alloc()))
        return NULL;

    obj->base.type = SS_OBJ_MUTEX;
    obj->base.u.mutex.owner = owner;
    if (owner) obj->base.u.mutex.count = 1;
    return obj;
}
