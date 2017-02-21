/*
 * lightweight keyed-events support
 *
 * Copyright (C) 2017 Daniel Lehman
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

#ifndef __WINE_KEVENTS_H
#define __WINE_KEVENTS_H

/* to be used in places where Win32 keyed events are used
 * Win32 keyed events use wineserver messages, which are a bottleneck
 * they can be alertable, which means they have to check the wineserver
 * internally, Wine uses keyed events but does not need them alertable
 * these lightweight ones use futexes and are intended to be used
 * in-proc only.  in cannot replace original code - these functions
 * should ONLY be called if lw_ke_supported() is non-zero and fall-back
 * to normal Keyed Events otherwise:
 *
 * change this code:
 * NtCreateKeyedEvent(&keyed_event, GENERIC_READ|GENERIC_WRITE, NULL, 0);
 * ...
 * NtWaitForKeyedEvent(keyed_event, &cv->Ptr, FALSE, &timeout);
 * ...
 * NtReleaseKeyedEvent(keyed_event, &cv->Ptr, FALSE, &timeout);
 * (note 3rd argument of 'FALSE' - lightweight keyed event is ONLY for this)
 *
 * to:
 * static lw_ke ke = LW_KE_INIT; // no equivalient for NtCreateKeyedEvent
 * ...
 * LW_KE_WAIT(&ke, keyed_event, &cv->Ptr, &timeout);
 *  ...
 * LW_KE_RELEASE(&ke, keyed_event, &cv->Ptr, &timeout);
 *
 * using the macros is preferred since it calls the lw_ke_supported()
 */

#define LW_KE_INIT      {0}

#ifdef __linux__

#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif
#include <errno.h>
#include "wine/list.h"

typedef struct
{
    int lock;
    struct list que;
} lw_ke_bin;

#define LW_KE_SHIFT 6
#define LW_KE_SIZE  (1<<LW_KE_SHIFT)

typedef struct
{
    int state;
    lw_ke_bin bins[LW_KE_SIZE];
} lw_ke;

extern long int syscall (long int __sysno, ...);
static inline int lw_ke_supported(void)
{
    static int supported = -1;

    if (supported == -1)
    {
        /*FUTEX_WAIT|FUTEX_PRIVATE_FLAG*/
        syscall( __NR_futex, &supported, 128, 10, NULL, 0, 0 );
        if (errno == ENOSYS)
        {
            /*FUTEX_WAIT*/
            syscall( __NR_futex, &supported, 0, 10, NULL, 0, 0 );
        }
        supported = (errno != ENOSYS);
    }
    return supported;
}

int lw_ke_wait(lw_ke *ke, const void *key, const long long *ntto);
int lw_ke_release(lw_ke *ke, const void *key, const long long *ntto);
#else
typedef struct
{
    int state;
} lw_ke;

static inline int lw_ke_supported(void) { return 0; }

int lw_ke_wait(lw_ke *ke, const void *key, const long long *ntto) { return ENOSYS; }
int lw_ke_release(lw_ke *ke, const void *key, const long long *ntto) { return ENOSYS; }
#endif

#define LW_KE_WAIT(lw, real, ptr, nt)                               \
        ({NTSTATUS ret;                                             \
        if (lw_ke_supported())                                      \
            ret = lw_ke_wait((lw), (ptr), (const long long *)(nt)); \
        else                                                        \
            ret = NtWaitForKeyedEvent((real), (ptr), FALSE, (nt));  \
        ret; })

#define LW_KE_RELEASE(lw, real, ptr, nt)                                \
        ({NTSTATUS ret;                                                 \
        if (lw_ke_supported())                                          \
            ret = lw_ke_release((lw), (ptr), (const long long *)(nt));  \
        else                                                            \
            ret = NtReleaseKeyedEvent((real), (ptr), FALSE, (nt));      \
        ret; })

#endif  /* __WINE_KEVENTS_H */
