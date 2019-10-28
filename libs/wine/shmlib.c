/*
 * Shared Memory functions
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

#include <sys/ipc.h>
#include <sys/shm.h>

#include "wine/shmlib.h"

/* TODO: what should be in server vs libwine?
   client-side has extra step of having to attach */
struct shm_segment;
struct shm_block;

#define MAX_SHM_SEGS_BITS   6
#define MAX_SHM_SEGS_SHIFT  (sizeof(shm_ptr_t) * 8 - MAX_SHM_SEGS_BITS)
#define MAX_SHM_SEGS        (1<<MAX_SHM_SEGS_BITS)
#define MAX_SHM_SEGS_MASK   ((long)(MAX_SHM_SEGS-1) << MAX_SHM_SEGS_SHIFT)

#define MAX_SHM_OFFSET_BITS MAX_SHM_SEGS_SHIFT
#define MAX_SHM_OFFSET      (1<<MAX_SHM_OFFSET_BITS)
#define MAX_SHM_OFFSET_MASK (MAX_SHM_OFFSET - 1)

struct shm_header
{
    unsigned int flags :  2;
    unsigned int size  : 30;
};

static struct shm_segment *segments[MAX_SHM_SEGS];

extern const char *wine_get_config_dir(void);
static inline key_t shm_get_key(int proj_id)
{
    return ftok(wine_get_config_dir(), proj_id);
}

static inline unsigned int segment_idx_from_ptr(shm_ptr_t shm_ptr)
{
    return shm_ptr >> MAX_SHM_SEGS_SHIFT;
}

static inline struct shm_block *shm_ptr_to_block(struct shm_segment *segment, shm_ptr_t shm_ptr)
{
    return (struct shm_block *)((char *)segment +
            (shm_ptr & MAX_SHM_OFFSET_MASK) - sizeof(struct shm_header));
}

static inline void *block_to_data_ptr(struct shm_block *block)
{
    return (char*)block + sizeof(struct shm_header);
}

static struct shm_segment *shm_attach(int idx)
{
    key_t key;
    int id;

    key = shm_get_key(idx);
    id = shmget(key, 0, 0);
    if (id == -1)
        return NULL;

    return shmat(id, NULL, 0);
}

void *shm_ptr_to_void_ptr(shm_ptr_t shmptr)
{
    struct shm_block *block;
    unsigned int shmidx;

    if (shmptr == SHM_NULL)
        return NULL;

    /* TODO: protect segments[] with mutex? */
    shmidx = segment_idx_from_ptr(shmptr);
    if (shmidx >= MAX_SHM_SEGS)
        return NULL;
    if (!segments[shmidx] &&
        !(segments[shmidx] = shm_attach(shmidx)))
        return NULL;

    block = shm_ptr_to_block(segments[shmidx], shmptr);
    return block_to_data_ptr(block);
}
