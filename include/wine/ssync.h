/*
 * Common shared object management
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

#ifndef SSYNC_H
#define SSYNC_H

enum ss_obj_type
{
    SS_OBJ_UNKNOWN,
    SS_OBJ_MUTEX,
    SS_OBJ_EVENT,
    SS_OBJ_SEMAPHORE,
};

struct ss_obj_mutex
{
    unsigned int    owner;  /* ptid */
    unsigned int    count;
    int             abandoned;
};

struct ss_obj_event
{
    int manual_reset;
    int signaled;
};

struct ss_obj_semaphore
{
    unsigned int count;
    unsigned int maxim;
};

struct ss_obj_base
{
    int                 lock;       /* shared across process */
    enum ss_obj_type    type;
    union
    {
        struct ss_obj_mutex     mutex;
        struct ss_obj_event     event;
        struct ss_obj_semaphore semaphore;
    } u;
};

#endif
