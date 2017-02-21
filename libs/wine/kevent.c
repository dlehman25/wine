/*
 * Lightweight keyed event support
 *
 * Copyright 2017 Daniel Lehman
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

#ifdef __linux__
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <syscall.h>

#include "wine/list.h"
#include "wine/kevent.h"

static int wait_op = 128; /* FUTEX_WAIT|FUTEX_PRIVATE_FLAG; */
static int wake_op = 129; /* FUTEX_WAKE|FUTEX_PRIVATE_FLAG; */

static inline int futex_wait( int *addr, int val, struct timespec *timeout )
{
    return syscall( SYS_futex, addr, wait_op, val, timeout, 0, 0 );
}

static inline int futex_wake( int *addr, int val )
{
    return syscall( SYS_futex, addr, wake_op, val, NULL, 0, 0 );
}

/* from Ulrich Drepper's "Futexes are Tricky" */
static inline void fast_lock( int *addr )
{
    int c;
    if ((c = interlocked_cmpxchg(addr, 1, 0)) != 0)
    {
        if (c != 2)
            c = interlocked_xchg(addr, 2);
        while (c != 0)
        {
            futex_wait(addr, 2, NULL);
            c = interlocked_xchg(addr, 2);
        }
    }
}

static inline void fast_unlock( int *addr )
{
    if (interlocked_xchg_add(addr, -1) != 1)
    {
        *addr = 0;
        futex_wake(addr, 1);
    }
}

#define LW_KE_OP_WAIT  1
#define LW_KE_OP_WAKE  2

typedef struct
{
    struct list entry;
    const void *key;
    int op;
    int go;
} ke_node;

#define LW_KE_UNINITIALIZED 0 /* MUST be zero */
#define LW_KE_INITIALIZED   1
#define LW_KE_INITIALIZING  2

static inline int lw_ke_valid(const lw_ke *ke)
{
    return *(volatile int *)&ke->state == LW_KE_INITIALIZED;
}

static void lw_ke_init_once(lw_ke *ke)
{
    int rc;
    int level;
    unsigned int i;

    rc = interlocked_cmpxchg(&ke->state, LW_KE_INITIALIZING, LW_KE_UNINITIALIZED);
    switch (rc)
    {
        case LW_KE_INITIALIZED:
            break;
        case LW_KE_UNINITIALIZED:
            for (i = 0; i < LW_KE_SIZE; i++)
            {
                ke->bins[i].lock = 0;
                list_init(&ke->bins[i].que);
            }
            /* lw_ke_supported should have been called by this time to determine
               if futexes are available.  this gets the level of support */
            level = 0;
            futex_wait(&level, 10, NULL);
            if (errno == ENOSYS)
            {
                wait_op = 0; /*FUTEX_WAIT*/
                wake_op = 1; /*FUTEX_WAKE*/
                futex_wait(&level, 10, NULL);
            }
            interlocked_xchg(&ke->state, LW_KE_INITIALIZED);
            futex_wake(&ke->state, INT_MAX);
            break;
        case LW_KE_INITIALIZING:
            do
            {
               futex_wait(&ke->state, LW_KE_INITIALIZING, NULL);
            } while (!lw_ke_valid(ke));
            break;
    }
}

/* less optimized pointer hash adapted from Linux include/linux/hash.h */

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32   0x9e370001ul
/* 2^63 + 2^61 - 2^57 + 2^54 - 2^51 - 2^18 + 1 */
#define GOLDEN_RATIO_PRIME_64   0x9e37fffffffc0001ul

static inline unsigned long lw_ke_hash(const void *key)
{
    const unsigned long prime = sizeof(void*) == 8 ? GOLDEN_RATIO_PRIME_64 : GOLDEN_RATIO_PRIME_32;
    unsigned long hash = (unsigned long)key * prime;
    return hash >> ((sizeof(hash) * 8) - LW_KE_SHIFT);
}

static inline int lw_ke_matching_op(int op)
{
    return op ^ (LW_KE_OP_WAIT ^ LW_KE_OP_WAKE);
}

static int lw_ke_futex_wait(int *addr, int val, const long long *ntto)
{
    int rc;
    long long rel;
    struct timespec timeout;

    if (!ntto)
        rc = futex_wait(addr, val, NULL);
    else
    {
        rel = -*ntto;
        timeout.tv_sec = rel / 10000000;
        timeout.tv_nsec = rel % 10000000;
        rc = futex_wait(addr, val, &timeout);
    }

    return rc == -1 ? errno : 0;
}

static int lw_ke_op(lw_ke *ke, const void *key, int op, const long long *ntto)
{
    lw_ke_bin *bin;
    ke_node *node;
    ke_node self;
    int rc;

    if (!lw_ke_valid(ke))
        lw_ke_init_once(ke);

    bin = &ke->bins[lw_ke_hash(key)];

    fast_lock(&bin->lock);
    LIST_FOR_EACH_ENTRY(node, &bin->que, ke_node, entry)
    {
        if (node->key != key) continue; /* multiple keys can hash to same bin */
        if (node->op != op) continue;
        node->go = 1;
        list_remove(&node->entry);
        fast_unlock(&bin->lock);

        futex_wake(&node->go, 1);
        return 0;
    }

    self.go = 0;
    self.op = lw_ke_matching_op(op);
    self.key = key;
    list_add_tail(&bin->que, &self.entry);
    fast_unlock(&bin->lock);

    do
    {
        rc = lw_ke_futex_wait(&self.go, 0, ntto);

        fast_lock(&bin->lock);
        if (rc == EAGAIN)
        {
            /* another thread picked us off the queue and set our go
               before we had a chance to wait */
            if (self.go)
                rc = 0;
        }
        else if (rc == ETIMEDOUT)
        {
            if (self.go)
                rc = 0; /* another thread signaled after timeout */
            else
            {
                list_remove(&self.entry);
                self.go = 1;
                rc = 0x102; /* STATUS_TIMEOUT */
            }
        }
        fast_unlock(&bin->lock);
    } while (!self.go);

    return rc;
}

int lw_ke_wait(lw_ke *ke, const void *key, const long long *ntto)
{
    return lw_ke_op(ke, key, LW_KE_OP_WAIT, ntto);
}

int lw_ke_release(lw_ke *ke, const void *key, const long long *ntto)
{
    return lw_ke_op(ke, key, LW_KE_OP_WAKE, ntto);
}
#endif
