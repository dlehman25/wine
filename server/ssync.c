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

#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdio.h>      // TODO:

struct ss_obj *ss_alloc(void)
{
    shm_ptr_t shm_ptr;
    struct ss_obj *obj;

    if ((shm_ptr = shm_malloc(sizeof(*obj))) == SHM_NULL)
        return NULL;

    obj = shm_ptr_to_void_ptr(shm_ptr);
    obj->shm_ptr = shm_ptr;
    return obj;
}

void ss_free(struct ss_obj *obj)
{
    if (!obj) return;
    shm_free(obj->shm_ptr);
}
