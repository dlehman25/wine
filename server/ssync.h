/*
 * Server-side shared object management
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

#ifndef SERVER_SSYNC_H
#define SERVER_SSYNC_H

#include "wine/ssync.h"

struct ss_obj
{
    struct ss_obj_base  base;
    shm_ptr_t           shm_ptr; /* TODO: remove */
};

struct ss_obj *ss_alloc(void);
void ss_free(struct ss_obj *);

#endif
