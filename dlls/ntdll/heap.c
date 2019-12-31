/*
 * Win32 heap functions
 *
 * Copyright 1996 Alexandre Julliard
 * Copyright 1998 Ulrich Weigand
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

#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#define LH_THREAD_SUPPORT
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "imagehlp.h"
#include "wine/list.h"
#include "wine/rbtree.h"
#include "wine/debug.h"
#include "wine/server.h"
#include "wine/unicode.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(heap);
WINE_DECLARE_DEBUG_CHANNEL(heapleaks);

struct tagHEAP;
static BOOL lh_enabled;
static void lh_stack_alloc(struct tagHEAP *, void *, SIZE_T);
static void lh_stack_free(struct tagHEAP *, void *);
static void lh_heap_create(struct tagHEAP *);
static void lh_heap_destroy(struct tagHEAP *);

/* Note: the heap data structures are loosely based on what Pietrek describes in his
 * book 'Windows 95 System Programming Secrets', with some adaptations for
 * better compatibility with NT.
 */

typedef struct tagARENA_INUSE
{
    DWORD  size;                    /* Block size; must be the first field */
    DWORD  magic : 24;              /* Magic number */
    DWORD  unused_bytes : 8;        /* Number of bytes in the block not used by user data (max value is HEAP_MIN_DATA_SIZE+HEAP_MIN_SHRINK_SIZE) */
} ARENA_INUSE;

typedef struct tagARENA_FREE
{
    DWORD                 size;     /* Block size; must be the first field */
    DWORD                 magic;    /* Magic number */
    struct list           entry;    /* Entry in free list */
} ARENA_FREE;

typedef struct
{
    struct list           entry;      /* entry in heap large blocks list */
    SIZE_T                data_size;  /* size of user data */
    SIZE_T                block_size; /* total size of virtual memory block */
    DWORD                 pad[2];     /* padding to ensure 16-byte alignment of data */
    DWORD                 size;       /* fields for compatibility with normal arenas */
    DWORD                 magic;      /* these must remain at the end of the structure */
} ARENA_LARGE;

#define ARENA_FLAG_FREE        0x00000001  /* flags OR'ed with arena size */
#define ARENA_FLAG_PREV_FREE   0x00000002
#define ARENA_SIZE_MASK        (~3)
#define ARENA_LARGE_SIZE       0xfedcba90  /* magic value for 'size' field in large blocks */

/* Value for arena 'magic' field */
#define ARENA_INUSE_MAGIC      0x455355
#define ARENA_PENDING_MAGIC    0xbedead
#define ARENA_FREE_MAGIC       0x45455246
#define ARENA_LARGE_MAGIC      0x6752614c

#define ARENA_INUSE_FILLER     0x55
#define ARENA_TAIL_FILLER      0xab
#define ARENA_FREE_FILLER      0xfeeefeee

/* everything is aligned on 8 byte boundaries (16 for Win64) */
#define ALIGNMENT              (2*sizeof(void*))
#define LARGE_ALIGNMENT        16  /* large blocks have stricter alignment */
#define ARENA_OFFSET           (ALIGNMENT - sizeof(ARENA_INUSE))

C_ASSERT( sizeof(ARENA_LARGE) % LARGE_ALIGNMENT == 0 );

#define ROUND_SIZE(size)       ((((size) + ALIGNMENT - 1) & ~(ALIGNMENT-1)) + ARENA_OFFSET)

#define QUIET                  1           /* Suppress messages  */
#define NOISY                  0           /* Report all errors  */

/* minimum data size (without arenas) of an allocated block */
/* make sure that it's larger than a free list entry */
#define HEAP_MIN_DATA_SIZE    ROUND_SIZE(2 * sizeof(struct list))
#define HEAP_MIN_ARENA_SIZE   (HEAP_MIN_DATA_SIZE + sizeof(ARENA_INUSE))
/* minimum size that must remain to shrink an allocated block */
#define HEAP_MIN_SHRINK_SIZE  (HEAP_MIN_DATA_SIZE+sizeof(ARENA_FREE))
/* minimum size to start allocating large blocks */
#define HEAP_MIN_LARGE_BLOCK_SIZE  0x7f000
/* extra size to add at the end of block for tail checking */
#define HEAP_TAIL_EXTRA_SIZE(flags) \
    ((flags & HEAP_TAIL_CHECKING_ENABLED) || RUNNING_ON_VALGRIND ? ALIGNMENT : 0)

/* There will be a free list bucket for every arena size up to and including this value */
#define HEAP_MAX_SMALL_FREE_LIST 0x100
C_ASSERT( HEAP_MAX_SMALL_FREE_LIST % ALIGNMENT == 0 );
#define HEAP_NB_SMALL_FREE_LISTS (((HEAP_MAX_SMALL_FREE_LIST - HEAP_MIN_ARENA_SIZE) / ALIGNMENT) + 1)

/* Max size of the blocks on the free lists above HEAP_MAX_SMALL_FREE_LIST */
static const SIZE_T HEAP_freeListSizes[] =
{
    0x200, 0x400, 0x1000, ~0UL
};
#define HEAP_NB_FREE_LISTS (ARRAY_SIZE( HEAP_freeListSizes ) + HEAP_NB_SMALL_FREE_LISTS)

typedef union
{
    ARENA_FREE  arena;
    void       *alignment[4];
} FREE_LIST_ENTRY;

struct tagHEAP;

typedef struct tagSUBHEAP
{
    void               *base;       /* Base address of the sub-heap memory block */
    SIZE_T              size;       /* Size of the whole sub-heap */
    SIZE_T              min_commit; /* Minimum committed size */
    SIZE_T              commitSize; /* Committed size of the sub-heap */
    struct list         entry;      /* Entry in sub-heap list */
    struct tagHEAP     *heap;       /* Main heap structure */
    DWORD               headerSize; /* Size of the heap header */
    DWORD               magic;      /* Magic number */
} SUBHEAP;

#define SUBHEAP_MAGIC    ((DWORD)('S' | ('U'<<8) | ('B'<<16) | ('H'<<24)))

typedef struct tagHEAP
{
    DWORD_PTR        unknown1[2];
    DWORD            unknown2;
    DWORD            flags;         /* Heap flags */
    DWORD            force_flags;   /* Forced heap flags for debugging */
    SUBHEAP          subheap;       /* First sub-heap */
    struct list      entry;         /* Entry in process heap list */
    struct list      subheap_list;  /* Sub-heap list */
    struct list      large_list;    /* Large blocks list */
    SIZE_T           grow_size;     /* Size of next subheap for growing heap */
    struct wine_rb_tree leaks;      /* Leak records (if enabled) */
    DWORD            magic;         /* Magic number */
    DWORD            pending_pos;   /* Position in pending free requests ring */
    ARENA_INUSE    **pending_free;  /* Ring buffer for pending free requests */
    RTL_CRITICAL_SECTION critSection; /* Critical section for serialization */
    FREE_LIST_ENTRY *freeList;      /* Free lists */
} HEAP;

#define HEAP_MAGIC       ((DWORD)('H' | ('E'<<8) | ('A'<<16) | ('P'<<24)))

#define HEAP_DEF_SIZE        0x110000   /* Default heap size = 1Mb + 64Kb */
#define COMMIT_MASK          0xffff  /* bitmask for commit/decommit granularity */
#define MAX_FREE_PENDING     1024    /* max number of free requests to delay */

/* some undocumented flags (names are made up) */
#define HEAP_PAGE_ALLOCS      0x01000000
#define HEAP_VALIDATE         0x10000000
#define HEAP_VALIDATE_ALL     0x20000000
#define HEAP_VALIDATE_PARAMS  0x40000000

static HEAP *processHeap;  /* main process heap */

static BOOL HEAP_IsRealArena( HEAP *heapPtr, DWORD flags, LPCVOID block, BOOL quiet );

/* mark a block of memory as free for debugging purposes */
static inline void mark_block_free( void *ptr, SIZE_T size, DWORD flags )
{
    if (flags & HEAP_FREE_CHECKING_ENABLED)
    {
        SIZE_T i;
        for (i = 0; i < size / sizeof(DWORD); i++) ((DWORD *)ptr)[i] = ARENA_FREE_FILLER;
    }
#if defined(VALGRIND_MAKE_MEM_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_NOACCESS( ptr, size ));
#elif defined( VALGRIND_MAKE_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_NOACCESS( ptr, size ));
#endif
}

/* mark a block of memory as initialized for debugging purposes */
static inline void mark_block_initialized( void *ptr, SIZE_T size )
{
#if defined(VALGRIND_MAKE_MEM_DEFINED)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_DEFINED( ptr, size ));
#elif defined(VALGRIND_MAKE_READABLE)
    VALGRIND_DISCARD( VALGRIND_MAKE_READABLE( ptr, size ));
#endif
}

/* mark a block of memory as uninitialized for debugging purposes */
static inline void mark_block_uninitialized( void *ptr, SIZE_T size )
{
#if defined(VALGRIND_MAKE_MEM_UNDEFINED)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_UNDEFINED( ptr, size ));
#elif defined(VALGRIND_MAKE_WRITABLE)
    VALGRIND_DISCARD( VALGRIND_MAKE_WRITABLE( ptr, size ));
#endif
}

/* mark a block of memory as a tail block */
static inline void mark_block_tail( void *ptr, SIZE_T size, DWORD flags )
{
    if (flags & HEAP_TAIL_CHECKING_ENABLED)
    {
        mark_block_uninitialized( ptr, size );
        memset( ptr, ARENA_TAIL_FILLER, size );
    }
#if defined(VALGRIND_MAKE_MEM_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_NOACCESS( ptr, size ));
#elif defined( VALGRIND_MAKE_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_NOACCESS( ptr, size ));
#endif
}

/* initialize contents of a newly created block of memory */
static inline void initialize_block( void *ptr, SIZE_T size, SIZE_T unused, DWORD flags )
{
    if (flags & HEAP_ZERO_MEMORY)
    {
        mark_block_initialized( ptr, size );
        memset( ptr, 0, size );
    }
    else
    {
        mark_block_uninitialized( ptr, size );
        if (flags & HEAP_FREE_CHECKING_ENABLED)
        {
            memset( ptr, ARENA_INUSE_FILLER, size );
            mark_block_uninitialized( ptr, size );
        }
    }

    mark_block_tail( (char *)ptr + size, unused, flags );
}

/* notify that a new block of memory has been allocated for debugging purposes */
static inline void notify_alloc( void *ptr, SIZE_T size, BOOL init )
{
#ifdef VALGRIND_MALLOCLIKE_BLOCK
    VALGRIND_MALLOCLIKE_BLOCK( ptr, size, 0, init );
#endif
}

/* notify that a block of memory has been freed for debugging purposes */
static inline void notify_free( void const *ptr )
{
#ifdef VALGRIND_FREELIKE_BLOCK
    VALGRIND_FREELIKE_BLOCK( ptr, 0 );
#endif
}

static inline void notify_realloc( void const *ptr, SIZE_T size_old, SIZE_T size_new )
{
#ifdef VALGRIND_RESIZEINPLACE_BLOCK
    /* zero is not a valid size */
    VALGRIND_RESIZEINPLACE_BLOCK( ptr, size_old ? size_old : 1, size_new ? size_new : 1, 0 );
#endif
}

static void subheap_notify_free_all(SUBHEAP const *subheap)
{
#ifdef VALGRIND_FREELIKE_BLOCK
    char const *ptr = (char const *)subheap->base + subheap->headerSize;

    if (!RUNNING_ON_VALGRIND) return;

    while (ptr < (char const *)subheap->base + subheap->size)
    {
        if (*(const DWORD *)ptr & ARENA_FLAG_FREE)
        {
            ARENA_FREE const *pArena = (ARENA_FREE const *)ptr;
            if (pArena->magic!=ARENA_FREE_MAGIC) ERR("bad free_magic @%p\n", pArena);
            ptr += sizeof(*pArena) + (pArena->size & ARENA_SIZE_MASK);
        }
        else
        {
            ARENA_INUSE const *pArena = (ARENA_INUSE const *)ptr;
            if (pArena->magic == ARENA_INUSE_MAGIC) notify_free(pArena + 1);
            else if (pArena->magic != ARENA_PENDING_MAGIC) ERR("bad inuse_magic @%p\n", pArena);
            ptr += sizeof(*pArena) + (pArena->size & ARENA_SIZE_MASK);
        }
    }
#endif
}

/* locate a free list entry of the appropriate size */
/* size is the size of the whole block including the arena header */
static inline unsigned int get_freelist_index( SIZE_T size )
{
    unsigned int i;

    if (size <= HEAP_MAX_SMALL_FREE_LIST)
        return (size - HEAP_MIN_ARENA_SIZE) / ALIGNMENT;

    for (i = HEAP_NB_SMALL_FREE_LISTS; i < HEAP_NB_FREE_LISTS - 1; i++)
        if (size <= HEAP_freeListSizes[i - HEAP_NB_SMALL_FREE_LISTS]) break;
    return i;
}

/* get the memory protection type to use for a given heap */
static inline ULONG get_protection_type( DWORD flags )
{
    return (flags & HEAP_CREATE_ENABLE_EXECUTE) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
}

static RTL_CRITICAL_SECTION_DEBUG process_heap_critsect_debug =
{
    0, 0, NULL,  /* will be set later */
    { &process_heap_critsect_debug.ProcessLocksList, &process_heap_critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": main process heap section") }
};


/***********************************************************************
 *           HEAP_Dump
 */
static void HEAP_Dump( HEAP *heap )
{
    unsigned int i;
    SUBHEAP *subheap;
    char *ptr;

    TRACE( "Heap: %p\n", heap );
    TRACE( "Next: %p  Sub-heaps:", LIST_ENTRY( heap->entry.next, HEAP, entry ) );
    LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry ) TRACE( " %p", subheap );

    TRACE( "\nFree lists:\n Block   Stat   Size    Id\n" );
    for (i = 0; i < HEAP_NB_FREE_LISTS; i++)
        TRACE( "%p free %08lx prev=%p next=%p\n",
                 &heap->freeList[i].arena, i < HEAP_NB_SMALL_FREE_LISTS ?
                 HEAP_MIN_ARENA_SIZE + i * ALIGNMENT : HEAP_freeListSizes[i - HEAP_NB_SMALL_FREE_LISTS],
                 LIST_ENTRY( heap->freeList[i].arena.entry.prev, ARENA_FREE, entry ),
                 LIST_ENTRY( heap->freeList[i].arena.entry.next, ARENA_FREE, entry ));

    LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry )
    {
        SIZE_T freeSize = 0, usedSize = 0, arenaSize = subheap->headerSize;
        TRACE( "\n\nSub-heap %p: base=%p size=%08lx committed=%08lx\n",
                 subheap, subheap->base, subheap->size, subheap->commitSize );

        TRACE( "\n Block    Arena   Stat   Size    Id\n" );
        ptr = (char *)subheap->base + subheap->headerSize;
        while (ptr < (char *)subheap->base + subheap->size)
        {
            if (*(DWORD *)ptr & ARENA_FLAG_FREE)
            {
                ARENA_FREE *pArena = (ARENA_FREE *)ptr;
                TRACE( "%p %08x free %08x prev=%p next=%p\n",
                         pArena, pArena->magic,
                         pArena->size & ARENA_SIZE_MASK,
                         LIST_ENTRY( pArena->entry.prev, ARENA_FREE, entry ),
                         LIST_ENTRY( pArena->entry.next, ARENA_FREE, entry ) );
                ptr += sizeof(*pArena) + (pArena->size & ARENA_SIZE_MASK);
                arenaSize += sizeof(ARENA_FREE);
                freeSize += pArena->size & ARENA_SIZE_MASK;
            }
            else if (*(DWORD *)ptr & ARENA_FLAG_PREV_FREE)
            {
                ARENA_INUSE *pArena = (ARENA_INUSE *)ptr;
                TRACE( "%p %08x Used %08x back=%p\n",
                        pArena, pArena->magic, pArena->size & ARENA_SIZE_MASK, *((ARENA_FREE **)pArena - 1) );
                ptr += sizeof(*pArena) + (pArena->size & ARENA_SIZE_MASK);
                arenaSize += sizeof(ARENA_INUSE);
                usedSize += pArena->size & ARENA_SIZE_MASK;
            }
            else
            {
                ARENA_INUSE *pArena = (ARENA_INUSE *)ptr;
                TRACE( "%p %08x %s %08x\n",
                         pArena, pArena->magic, pArena->magic == ARENA_INUSE_MAGIC ? "used" : "pend",
                         pArena->size & ARENA_SIZE_MASK );
                ptr += sizeof(*pArena) + (pArena->size & ARENA_SIZE_MASK);
                arenaSize += sizeof(ARENA_INUSE);
                usedSize += pArena->size & ARENA_SIZE_MASK;
            }
        }
        TRACE( "\nTotal: Size=%08lx Committed=%08lx Free=%08lx Used=%08lx Arenas=%08lx (%ld%%)\n\n",
	      subheap->size, subheap->commitSize, freeSize, usedSize,
	      arenaSize, (arenaSize * 100) / subheap->size );
    }
}


static void HEAP_DumpEntry( LPPROCESS_HEAP_ENTRY entry )
{
    WORD rem_flags;
    TRACE( "Dumping entry %p\n", entry );
    TRACE( "lpData\t\t: %p\n", entry->lpData );
    TRACE( "cbData\t\t: %08x\n", entry->cbData);
    TRACE( "cbOverhead\t: %08x\n", entry->cbOverhead);
    TRACE( "iRegionIndex\t: %08x\n", entry->iRegionIndex);
    TRACE( "WFlags\t\t: ");
    if (entry->wFlags & PROCESS_HEAP_REGION)
        TRACE( "PROCESS_HEAP_REGION ");
    if (entry->wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE)
        TRACE( "PROCESS_HEAP_UNCOMMITTED_RANGE ");
    if (entry->wFlags & PROCESS_HEAP_ENTRY_BUSY)
        TRACE( "PROCESS_HEAP_ENTRY_BUSY ");
    if (entry->wFlags & PROCESS_HEAP_ENTRY_MOVEABLE)
        TRACE( "PROCESS_HEAP_ENTRY_MOVEABLE ");
    if (entry->wFlags & PROCESS_HEAP_ENTRY_DDESHARE)
        TRACE( "PROCESS_HEAP_ENTRY_DDESHARE ");
    rem_flags = entry->wFlags &
        ~(PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE |
          PROCESS_HEAP_ENTRY_BUSY | PROCESS_HEAP_ENTRY_MOVEABLE|
          PROCESS_HEAP_ENTRY_DDESHARE);
    if (rem_flags)
        TRACE( "Unknown %08x", rem_flags);
    TRACE( "\n");
    if ((entry->wFlags & PROCESS_HEAP_ENTRY_BUSY )
        && (entry->wFlags & PROCESS_HEAP_ENTRY_MOVEABLE))
    {
        /* Treat as block */
        TRACE( "BLOCK->hMem\t\t:%p\n", entry->u.Block.hMem);
    }
    if (entry->wFlags & PROCESS_HEAP_REGION)
    {
        TRACE( "Region.dwCommittedSize\t:%08x\n",entry->u.Region.dwCommittedSize);
        TRACE( "Region.dwUnCommittedSize\t:%08x\n",entry->u.Region.dwUnCommittedSize);
        TRACE( "Region.lpFirstBlock\t:%p\n",entry->u.Region.lpFirstBlock);
        TRACE( "Region.lpLastBlock\t:%p\n",entry->u.Region.lpLastBlock);
    }
}

/***********************************************************************
 *           HEAP_GetPtr
 * RETURNS
 *	Pointer to the heap
 *	NULL: Failure
 */
static HEAP *HEAP_GetPtr(
             HANDLE heap /* [in] Handle to the heap */
) {
    HEAP *heapPtr = heap;
    if (!heapPtr || (heapPtr->magic != HEAP_MAGIC))
    {
        ERR("Invalid heap %p!\n", heap );
        return NULL;
    }
    if ((heapPtr->flags & HEAP_VALIDATE_ALL) && !HEAP_IsRealArena( heapPtr, 0, NULL, NOISY ))
    {
        if (TRACE_ON(heap))
        {
            HEAP_Dump( heapPtr );
            assert( FALSE );
        }
        return NULL;
    }
    return heapPtr;
}


/***********************************************************************
 *           HEAP_InsertFreeBlock
 *
 * Insert a free block into the free list.
 */
static inline void HEAP_InsertFreeBlock( HEAP *heap, ARENA_FREE *pArena, BOOL last )
{
    FREE_LIST_ENTRY *pEntry = heap->freeList + get_freelist_index( pArena->size + sizeof(*pArena) );
    if (last)
    {
        /* insert at end of free list, i.e. before the next free list entry */
        pEntry++;
        if (pEntry == &heap->freeList[HEAP_NB_FREE_LISTS]) pEntry = heap->freeList;
        list_add_before( &pEntry->arena.entry, &pArena->entry );
    }
    else
    {
        /* insert at head of free list */
        list_add_after( &pEntry->arena.entry, &pArena->entry );
    }
    pArena->size |= ARENA_FLAG_FREE;
}


/***********************************************************************
 *           HEAP_FindSubHeap
 * Find the sub-heap containing a given address.
 *
 * RETURNS
 *	Pointer: Success
 *	NULL: Failure
 */
static SUBHEAP *HEAP_FindSubHeap(
                const HEAP *heap, /* [in] Heap pointer */
                LPCVOID ptr ) /* [in] Address */
{
    SUBHEAP *sub;
    LIST_FOR_EACH_ENTRY( sub, &heap->subheap_list, SUBHEAP, entry )
        if ((ptr >= sub->base) &&
            ((const char *)ptr < (const char *)sub->base + sub->size - sizeof(ARENA_INUSE)))
            return sub;
    return NULL;
}


/***********************************************************************
 *           HEAP_Commit
 *
 * Make sure the heap storage is committed for a given size in the specified arena.
 */
static inline BOOL HEAP_Commit( SUBHEAP *subheap, ARENA_INUSE *pArena, SIZE_T data_size )
{
    void *ptr = (char *)(pArena + 1) + data_size + sizeof(ARENA_FREE);
    SIZE_T size = (char *)ptr - (char *)subheap->base;
    size = (size + COMMIT_MASK) & ~COMMIT_MASK;
    if (size > subheap->size) size = subheap->size;
    if (size <= subheap->commitSize) return TRUE;
    size -= subheap->commitSize;
    ptr = (char *)subheap->base + subheap->commitSize;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), &ptr, 0,
                                 &size, MEM_COMMIT, get_protection_type( subheap->heap->flags ) ))
    {
        WARN("Could not commit %08lx bytes at %p for heap %p\n",
                 size, ptr, subheap->heap );
        return FALSE;
    }
    subheap->commitSize += size;
    return TRUE;
}


/***********************************************************************
 *           HEAP_Decommit
 *
 * If possible, decommit the heap storage from (including) 'ptr'.
 */
static inline BOOL HEAP_Decommit( SUBHEAP *subheap, void *ptr )
{
    void *addr;
    SIZE_T decommit_size;
    SIZE_T size = (char *)ptr - (char *)subheap->base;

    /* round to next block and add one full block */
    size = ((size + COMMIT_MASK) & ~COMMIT_MASK) + COMMIT_MASK + 1;
    size = max( size, subheap->min_commit );
    if (size >= subheap->commitSize) return TRUE;
    decommit_size = subheap->commitSize - size;
    addr = (char *)subheap->base + size;

    if (NtFreeVirtualMemory( NtCurrentProcess(), &addr, &decommit_size, MEM_DECOMMIT ))
    {
        WARN("Could not decommit %08lx bytes at %p for heap %p\n",
             decommit_size, (char *)subheap->base + size, subheap->heap );
        return FALSE;
    }
    subheap->commitSize -= decommit_size;
    return TRUE;
}


/***********************************************************************
 *           HEAP_CreateFreeBlock
 *
 * Create a free block at a specified address. 'size' is the size of the
 * whole block, including the new arena.
 */
static void HEAP_CreateFreeBlock( SUBHEAP *subheap, void *ptr, SIZE_T size )
{
    ARENA_FREE *pFree;
    char *pEnd;
    BOOL last;
    DWORD flags = subheap->heap->flags;

    /* Create a free arena */
    mark_block_uninitialized( ptr, sizeof(ARENA_FREE) );
    pFree = ptr;
    pFree->magic = ARENA_FREE_MAGIC;

    /* If debugging, erase the freed block content */

    pEnd = (char *)ptr + size;
    if (pEnd > (char *)subheap->base + subheap->commitSize)
        pEnd = (char *)subheap->base + subheap->commitSize;
    if (pEnd > (char *)(pFree + 1)) mark_block_free( pFree + 1, pEnd - (char *)(pFree + 1), flags );

    /* Check if next block is free also */

    if (((char *)ptr + size < (char *)subheap->base + subheap->size) &&
        (*(DWORD *)((char *)ptr + size) & ARENA_FLAG_FREE))
    {
        /* Remove the next arena from the free list */
        ARENA_FREE *pNext = (ARENA_FREE *)((char *)ptr + size);
        list_remove( &pNext->entry );
        size += (pNext->size & ARENA_SIZE_MASK) + sizeof(*pNext);
        mark_block_free( pNext, sizeof(ARENA_FREE), flags );
    }

    /* Set the next block PREV_FREE flag and pointer */

    last = ((char *)ptr + size >= (char *)subheap->base + subheap->size);
    if (!last)
    {
        DWORD *pNext = (DWORD *)((char *)ptr + size);
        *pNext |= ARENA_FLAG_PREV_FREE;
        mark_block_initialized( (ARENA_FREE **)pNext - 1, sizeof( ARENA_FREE * ) );
        *((ARENA_FREE **)pNext - 1) = pFree;
    }

    /* Last, insert the new block into the free list */

    pFree->size = size - sizeof(*pFree);
    HEAP_InsertFreeBlock( subheap->heap, pFree, last );
}


/***********************************************************************
 *           HEAP_MakeInUseBlockFree
 *
 * Turn an in-use block into a free block. Can also decommit the end of
 * the heap, and possibly even free the sub-heap altogether.
 */
static void HEAP_MakeInUseBlockFree( SUBHEAP *subheap, ARENA_INUSE *pArena )
{
    HEAP *heap = subheap->heap;
    ARENA_FREE *pFree;
    SIZE_T size;

    if (heap->pending_free)
    {
        ARENA_INUSE *prev = heap->pending_free[heap->pending_pos];
        heap->pending_free[heap->pending_pos] = pArena;
        heap->pending_pos = (heap->pending_pos + 1) % MAX_FREE_PENDING;
        pArena->magic = ARENA_PENDING_MAGIC;
        mark_block_free( pArena + 1, pArena->size & ARENA_SIZE_MASK, heap->flags );
        if (!prev) return;
        pArena = prev;
        subheap = HEAP_FindSubHeap( heap, pArena );
    }

    /* Check if we can merge with previous block */

    size = (pArena->size & ARENA_SIZE_MASK) + sizeof(*pArena);
    if (pArena->size & ARENA_FLAG_PREV_FREE)
    {
        pFree = *((ARENA_FREE **)pArena - 1);
        size += (pFree->size & ARENA_SIZE_MASK) + sizeof(ARENA_FREE);
        /* Remove it from the free list */
        list_remove( &pFree->entry );
    }
    else pFree = (ARENA_FREE *)pArena;

    /* Create a free block */

    HEAP_CreateFreeBlock( subheap, pFree, size );
    size = (pFree->size & ARENA_SIZE_MASK) + sizeof(ARENA_FREE);
    if ((char *)pFree + size < (char *)subheap->base + subheap->size)
        return;  /* Not the last block, so nothing more to do */

    /* Free the whole sub-heap if it's empty and not the original one */

    if (((char *)pFree == (char *)subheap->base + subheap->headerSize) &&
        (subheap != &subheap->heap->subheap))
    {
        void *addr = subheap->base;

        size = 0;
        /* Remove the free block from the list */
        list_remove( &pFree->entry );
        /* Remove the subheap from the list */
        list_remove( &subheap->entry );
        /* Free the memory */
        subheap->magic = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
        return;
    }

    /* Decommit the end of the heap */

    if (!(subheap->heap->flags & HEAP_SHARED)) HEAP_Decommit( subheap, pFree + 1 );
}


/***********************************************************************
 *           HEAP_ShrinkBlock
 *
 * Shrink an in-use block.
 */
static void HEAP_ShrinkBlock(SUBHEAP *subheap, ARENA_INUSE *pArena, SIZE_T size)
{
    if ((pArena->size & ARENA_SIZE_MASK) >= size + HEAP_MIN_SHRINK_SIZE)
    {
        HEAP_CreateFreeBlock( subheap, (char *)(pArena + 1) + size,
                              (pArena->size & ARENA_SIZE_MASK) - size );
	/* assign size plus previous arena flags */
        pArena->size = size | (pArena->size & ~ARENA_SIZE_MASK);
    }
    else
    {
        /* Turn off PREV_FREE flag in next block */
        char *pNext = (char *)(pArena + 1) + (pArena->size & ARENA_SIZE_MASK);
        if (pNext < (char *)subheap->base + subheap->size)
            *(DWORD *)pNext &= ~ARENA_FLAG_PREV_FREE;
    }
}


/***********************************************************************
 *           allocate_large_block
 */
static void *allocate_large_block( HEAP *heap, DWORD flags, SIZE_T size )
{
    ARENA_LARGE *arena;
    SIZE_T block_size = sizeof(*arena) + ROUND_SIZE(size) + HEAP_TAIL_EXTRA_SIZE(flags);
    LPVOID address = NULL;

    if (block_size < size) return NULL;  /* overflow */
    if (virtual_alloc_aligned( &address, 0, &block_size, MEM_COMMIT, get_protection_type( flags ), 5 ))
    {
        WARN("Could not allocate block for %08lx bytes\n", size );
        return NULL;
    }
    arena = address;
    arena->data_size = size;
    arena->block_size = block_size;
    arena->size = ARENA_LARGE_SIZE;
    arena->magic = ARENA_LARGE_MAGIC;
    mark_block_tail( (char *)(arena + 1) + size, block_size - sizeof(*arena) - size, flags );
    list_add_tail( &heap->large_list, &arena->entry );
    notify_alloc( arena + 1, size, flags & HEAP_ZERO_MEMORY );
    return arena + 1;
}


/***********************************************************************
 *           free_large_block
 */
static void free_large_block( HEAP *heap, DWORD flags, void *ptr )
{
    ARENA_LARGE *arena = (ARENA_LARGE *)ptr - 1;
    LPVOID address = arena;
    SIZE_T size = 0;

    list_remove( &arena->entry );
    NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
}


/***********************************************************************
 *           realloc_large_block
 */
static void *realloc_large_block( HEAP *heap, DWORD flags, void *ptr, SIZE_T size )
{
    ARENA_LARGE *arena = (ARENA_LARGE *)ptr - 1;
    void *new_ptr;

    if (arena->block_size - sizeof(*arena) >= size)
    {
        SIZE_T unused = arena->block_size - sizeof(*arena) - size;

        /* FIXME: we could remap zero-pages instead */
#ifdef VALGRIND_RESIZEINPLACE_BLOCK
        if (RUNNING_ON_VALGRIND)
            notify_realloc( arena + 1, arena->data_size, size );
        else
#endif
        if (size > arena->data_size)
            initialize_block( (char *)ptr + arena->data_size, size - arena->data_size, unused, flags );
        else
            mark_block_tail( (char *)ptr + size, unused, flags );
        arena->data_size = size;
        return ptr;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return NULL;
    if (!(new_ptr = allocate_large_block( heap, flags, size )))
    {
        WARN("Could not allocate block for %08lx bytes\n", size );
        return NULL;
    }
    memcpy( new_ptr, ptr, arena->data_size );
    free_large_block( heap, flags, ptr );
    notify_free( ptr );
    return new_ptr;
}


/***********************************************************************
 *           find_large_block
 */
static ARENA_LARGE *find_large_block( HEAP *heap, const void *ptr )
{
    ARENA_LARGE *arena;

    LIST_FOR_EACH_ENTRY( arena, &heap->large_list, ARENA_LARGE, entry )
        if (ptr == arena + 1) return arena;

    return NULL;
}


/***********************************************************************
 *           validate_large_arena
 */
static BOOL validate_large_arena( HEAP *heap, const ARENA_LARGE *arena, BOOL quiet )
{
    DWORD flags = heap->flags;

    if ((ULONG_PTR)arena % page_size)
    {
        if (quiet == NOISY)
        {
            ERR( "Heap %p: invalid large arena pointer %p\n", heap, arena );
            if (TRACE_ON(heap)) HEAP_Dump( heap );
        }
        else if (WARN_ON(heap))
        {
            WARN( "Heap %p: unaligned arena pointer %p\n", heap, arena );
            if (TRACE_ON(heap)) HEAP_Dump( heap );
        }
        return FALSE;
    }
    if (arena->size != ARENA_LARGE_SIZE || arena->magic != ARENA_LARGE_MAGIC)
    {
        if (quiet == NOISY)
        {
            ERR( "Heap %p: invalid large arena %p values %x/%x\n",
                 heap, arena, arena->size, arena->magic );
            if (TRACE_ON(heap)) HEAP_Dump( heap );
        }
        else if (WARN_ON(heap))
        {
            WARN( "Heap %p: invalid large arena %p values %x/%x\n",
                  heap, arena, arena->size, arena->magic );
            if (TRACE_ON(heap)) HEAP_Dump( heap );
        }
        return FALSE;
    }
    if (arena->data_size > arena->block_size - sizeof(*arena))
    {
        ERR( "Heap %p: invalid large arena %p size %lx/%lx\n",
             heap, arena, arena->data_size, arena->block_size );
        return FALSE;
    }
    if (flags & HEAP_TAIL_CHECKING_ENABLED)
    {
        SIZE_T i, unused = arena->block_size - sizeof(*arena) - arena->data_size;
        const unsigned char *data = (const unsigned char *)(arena + 1) + arena->data_size;

        for (i = 0; i < unused; i++)
        {
            if (data[i] == ARENA_TAIL_FILLER) continue;
            ERR("Heap %p: block %p tail overwritten at %p (byte %lu/%lu == 0x%02x)\n",
                heap, arena + 1, data + i, i, unused, data[i] );
            return FALSE;
        }
    }
    return TRUE;
}


/***********************************************************************
 *           HEAP_CreateSubHeap
 */
static SUBHEAP *HEAP_CreateSubHeap( HEAP *heap, LPVOID address, DWORD flags,
                                    SIZE_T commitSize, SIZE_T totalSize )
{
    SUBHEAP *subheap;
    FREE_LIST_ENTRY *pEntry;
    unsigned int i;

    if (!address)
    {
        if (!commitSize) commitSize = COMMIT_MASK + 1;
        totalSize = min( totalSize, 0xffff0000 );  /* don't allow a heap larger than 4GB */
        if (totalSize < commitSize) totalSize = commitSize;
        if (flags & HEAP_SHARED) commitSize = totalSize;  /* always commit everything in a shared heap */
        commitSize = min( totalSize, (commitSize + COMMIT_MASK) & ~COMMIT_MASK );

        /* allocate the memory block */
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &totalSize,
                                     MEM_RESERVE, get_protection_type( flags ) ))
        {
            WARN("Could not allocate %08lx bytes\n", totalSize );
            return NULL;
        }
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0,
                                     &commitSize, MEM_COMMIT, get_protection_type( flags ) ))
        {
            WARN("Could not commit %08lx bytes for sub-heap %p\n", commitSize, address );
            return NULL;
        }
    }

    if (heap)
    {
        /* If this is a secondary subheap, insert it into list */

        subheap = address;
        subheap->base       = address;
        subheap->heap       = heap;
        subheap->size       = totalSize;
        subheap->min_commit = 0x10000;
        subheap->commitSize = commitSize;
        subheap->magic      = SUBHEAP_MAGIC;
        subheap->headerSize = ROUND_SIZE( sizeof(SUBHEAP) );
        list_add_head( &heap->subheap_list, &subheap->entry );
    }
    else
    {
        /* If this is a primary subheap, initialize main heap */

        heap = address;
        heap->flags         = flags;
        heap->magic         = HEAP_MAGIC;
        heap->grow_size     = max( HEAP_DEF_SIZE, totalSize );
        list_init( &heap->subheap_list );
        list_init( &heap->large_list );

        subheap = &heap->subheap;
        subheap->base       = address;
        subheap->heap       = heap;
        subheap->size       = totalSize;
        subheap->min_commit = commitSize;
        subheap->commitSize = commitSize;
        subheap->magic      = SUBHEAP_MAGIC;
        subheap->headerSize = ROUND_SIZE( sizeof(HEAP) );
        list_add_head( &heap->subheap_list, &subheap->entry );

        /* Build the free lists */

        heap->freeList = (FREE_LIST_ENTRY *)((char *)heap + subheap->headerSize);
        subheap->headerSize += HEAP_NB_FREE_LISTS * sizeof(FREE_LIST_ENTRY);
        list_init( &heap->freeList[0].arena.entry );
        for (i = 0, pEntry = heap->freeList; i < HEAP_NB_FREE_LISTS; i++, pEntry++)
        {
            pEntry->arena.size = 0 | ARENA_FLAG_FREE;
            pEntry->arena.magic = ARENA_FREE_MAGIC;
            if (i) list_add_after( &pEntry[-1].arena.entry, &pEntry->arena.entry );
        }

        /* Initialize critical section */

        if (!processHeap)  /* do it by hand to avoid memory allocations */
        {
            heap->critSection.DebugInfo      = &process_heap_critsect_debug;
            heap->critSection.LockCount      = -1;
            heap->critSection.RecursionCount = 0;
            heap->critSection.OwningThread   = 0;
            heap->critSection.LockSemaphore  = 0;
            heap->critSection.SpinCount      = 0;
            process_heap_critsect_debug.CriticalSection = &heap->critSection;
        }
        else
        {
            RtlInitializeCriticalSection( &heap->critSection );
            heap->critSection.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": HEAP.critSection");
        }

        if (flags & HEAP_SHARED)
        {
            /* let's assume that only one thread at a time will try to do this */
            HANDLE sem = heap->critSection.LockSemaphore;
            if (!sem) NtCreateSemaphore( &sem, SEMAPHORE_ALL_ACCESS, NULL, 0, 1 );

            NtDuplicateObject( NtCurrentProcess(), sem, NtCurrentProcess(), &sem, 0, 0,
                               DUP_HANDLE_MAKE_GLOBAL | DUP_HANDLE_SAME_ACCESS | DUP_HANDLE_CLOSE_SOURCE );
            heap->critSection.LockSemaphore = sem;
            RtlFreeHeap( processHeap, 0, heap->critSection.DebugInfo );
            heap->critSection.DebugInfo = NULL;
        }
    }

    /* Create the first free block */

    HEAP_CreateFreeBlock( subheap, (LPBYTE)subheap->base + subheap->headerSize,
                          subheap->size - subheap->headerSize );

    return subheap;
}


/***********************************************************************
 *           HEAP_FindFreeBlock
 *
 * Find a free block at least as large as the requested size, and make sure
 * the requested size is committed.
 */
static ARENA_FREE *HEAP_FindFreeBlock( HEAP *heap, SIZE_T size,
                                       SUBHEAP **ppSubHeap )
{
    SUBHEAP *subheap;
    struct list *ptr;
    SIZE_T total_size;
    FREE_LIST_ENTRY *pEntry = heap->freeList + get_freelist_index( size + sizeof(ARENA_INUSE) );

    /* Find a suitable free list, and in it find a block large enough */

    ptr = &pEntry->arena.entry;
    while ((ptr = list_next( &heap->freeList[0].arena.entry, ptr )))
    {
        ARENA_FREE *pArena = LIST_ENTRY( ptr, ARENA_FREE, entry );
        SIZE_T arena_size = (pArena->size & ARENA_SIZE_MASK) +
                            sizeof(ARENA_FREE) - sizeof(ARENA_INUSE);
        if (arena_size >= size)
        {
            subheap = HEAP_FindSubHeap( heap, pArena );
            if (!HEAP_Commit( subheap, (ARENA_INUSE *)pArena, size )) return NULL;
            *ppSubHeap = subheap;
            return pArena;
        }
    }

    /* If no block was found, attempt to grow the heap */

    if (!(heap->flags & HEAP_GROWABLE))
    {
        WARN("Not enough space in heap %p for %08lx bytes\n", heap, size );
        return NULL;
    }
    /* make sure that we have a big enough size *committed* to fit another
     * last free arena in !
     * So just one heap struct, one first free arena which will eventually
     * get used, and a second free arena that might get assigned all remaining
     * free space in HEAP_ShrinkBlock() */
    total_size = size + ROUND_SIZE(sizeof(SUBHEAP)) + sizeof(ARENA_INUSE) + sizeof(ARENA_FREE);
    if (total_size < size) return NULL;  /* overflow */

    if ((subheap = HEAP_CreateSubHeap( heap, NULL, heap->flags, total_size,
                                       max( heap->grow_size, total_size ) )))
    {
        if (heap->grow_size < 128 * 1024 * 1024) heap->grow_size *= 2;
    }
    else while (!subheap)  /* shrink the grow size again if we are running out of space */
    {
        if (heap->grow_size <= total_size || heap->grow_size <= 4 * 1024 * 1024) return NULL;
        heap->grow_size /= 2;
        subheap = HEAP_CreateSubHeap( heap, NULL, heap->flags, total_size,
                                      max( heap->grow_size, total_size ) );
    }

    TRACE("created new sub-heap %p of %08lx bytes for heap %p\n",
          subheap, subheap->size, heap );

    *ppSubHeap = subheap;
    return (ARENA_FREE *)((char *)subheap->base + subheap->headerSize);
}


/***********************************************************************
 *           HEAP_IsValidArenaPtr
 *
 * Check that the pointer is inside the range possible for arenas.
 */
static BOOL HEAP_IsValidArenaPtr( const HEAP *heap, const ARENA_FREE *ptr )
{
    unsigned int i;
    const SUBHEAP *subheap = HEAP_FindSubHeap( heap, ptr );
    if (!subheap) return FALSE;
    if ((const char *)ptr >= (const char *)subheap->base + subheap->headerSize) return TRUE;
    if (subheap != &heap->subheap) return FALSE;
    for (i = 0; i < HEAP_NB_FREE_LISTS; i++)
        if (ptr == &heap->freeList[i].arena) return TRUE;
    return FALSE;
}


/***********************************************************************
 *           HEAP_ValidateFreeArena
 */
static BOOL HEAP_ValidateFreeArena( SUBHEAP *subheap, ARENA_FREE *pArena )
{
    DWORD flags = subheap->heap->flags;
    SIZE_T size;
    ARENA_FREE *prev, *next;
    char *heapEnd = (char *)subheap->base + subheap->size;

    /* Check for unaligned pointers */
    if ((ULONG_PTR)pArena % ALIGNMENT != ARENA_OFFSET)
    {
        ERR("Heap %p: unaligned arena pointer %p\n", subheap->heap, pArena );
        return FALSE;
    }

    /* Check magic number */
    if (pArena->magic != ARENA_FREE_MAGIC)
    {
        ERR("Heap %p: invalid free arena magic %08x for %p\n", subheap->heap, pArena->magic, pArena );
        return FALSE;
    }
    /* Check size flags */
    if (!(pArena->size & ARENA_FLAG_FREE) ||
        (pArena->size & ARENA_FLAG_PREV_FREE))
    {
        ERR("Heap %p: bad flags %08x for free arena %p\n",
            subheap->heap, pArena->size & ~ARENA_SIZE_MASK, pArena );
        return FALSE;
    }
    /* Check arena size */
    size = pArena->size & ARENA_SIZE_MASK;
    if ((char *)(pArena + 1) + size > heapEnd)
    {
        ERR("Heap %p: bad size %08lx for free arena %p\n", subheap->heap, size, pArena );
        return FALSE;
    }
    /* Check that next pointer is valid */
    next = LIST_ENTRY( pArena->entry.next, ARENA_FREE, entry );
    if (!HEAP_IsValidArenaPtr( subheap->heap, next ))
    {
        ERR("Heap %p: bad next ptr %p for arena %p\n",
            subheap->heap, next, pArena );
        return FALSE;
    }
    /* Check that next arena is free */
    if (!(next->size & ARENA_FLAG_FREE) || (next->magic != ARENA_FREE_MAGIC))
    {
        ERR("Heap %p: next arena %p invalid for %p\n",
            subheap->heap, next, pArena );
        return FALSE;
    }
    /* Check that prev pointer is valid */
    prev = LIST_ENTRY( pArena->entry.prev, ARENA_FREE, entry );
    if (!HEAP_IsValidArenaPtr( subheap->heap, prev ))
    {
        ERR("Heap %p: bad prev ptr %p for arena %p\n",
            subheap->heap, prev, pArena );
        return FALSE;
    }
    /* Check that prev arena is free */
    if (!(prev->size & ARENA_FLAG_FREE) || (prev->magic != ARENA_FREE_MAGIC))
    {
	/* this often means that the prev arena got overwritten
	 * by a memory write before that prev arena */
        ERR("Heap %p: prev arena %p invalid for %p\n",
            subheap->heap, prev, pArena );
        return FALSE;
    }
    /* Check that next block has PREV_FREE flag */
    if ((char *)(pArena + 1) + size < heapEnd)
    {
        if (!(*(DWORD *)((char *)(pArena + 1) + size) & ARENA_FLAG_PREV_FREE))
        {
            ERR("Heap %p: free arena %p next block has no PREV_FREE flag\n",
                subheap->heap, pArena );
            return FALSE;
        }
        /* Check next block back pointer */
        if (*((ARENA_FREE **)((char *)(pArena + 1) + size) - 1) != pArena)
        {
            ERR("Heap %p: arena %p has wrong back ptr %p\n",
                subheap->heap, pArena,
                *((ARENA_FREE **)((char *)(pArena+1) + size) - 1));
            return FALSE;
        }
    }
    if (flags & HEAP_FREE_CHECKING_ENABLED)
    {
        DWORD *ptr = (DWORD *)(pArena + 1);
        char *end = (char *)(pArena + 1) + size;

        if (end >= heapEnd) end = (char *)subheap->base + subheap->commitSize;
        else end -= sizeof(ARENA_FREE *);
        while (ptr < (DWORD *)end)
        {
            if (*ptr != ARENA_FREE_FILLER)
            {
                ERR("Heap %p: free block %p overwritten at %p by %08x\n",
                    subheap->heap, (ARENA_INUSE *)pArena + 1, ptr, *ptr );
                return FALSE;
            }
            ptr++;
        }
    }
    return TRUE;
}


/***********************************************************************
 *           HEAP_ValidateInUseArena
 */
static BOOL HEAP_ValidateInUseArena( const SUBHEAP *subheap, const ARENA_INUSE *pArena, BOOL quiet )
{
    SIZE_T size;
    DWORD i, flags = subheap->heap->flags;
    const char *heapEnd = (const char *)subheap->base + subheap->size;

    /* Check for unaligned pointers */
    if ((ULONG_PTR)pArena % ALIGNMENT != ARENA_OFFSET)
    {
        if ( quiet == NOISY )
        {
            ERR( "Heap %p: unaligned arena pointer %p\n", subheap->heap, pArena );
            if ( TRACE_ON(heap) )
                HEAP_Dump( subheap->heap );
        }
        else if ( WARN_ON(heap) )
        {
            WARN( "Heap %p: unaligned arena pointer %p\n", subheap->heap, pArena );
            if ( TRACE_ON(heap) )
                HEAP_Dump( subheap->heap );
        }
        return FALSE;
    }

    /* Check magic number */
    if (pArena->magic != ARENA_INUSE_MAGIC && pArena->magic != ARENA_PENDING_MAGIC)
    {
        if (quiet == NOISY) {
            ERR("Heap %p: invalid in-use arena magic %08x for %p\n", subheap->heap, pArena->magic, pArena );
            if (TRACE_ON(heap))
               HEAP_Dump( subheap->heap );
        }  else if (WARN_ON(heap)) {
            WARN("Heap %p: invalid in-use arena magic %08x for %p\n", subheap->heap, pArena->magic, pArena );
            if (TRACE_ON(heap))
               HEAP_Dump( subheap->heap );
        }
        return FALSE;
    }
    /* Check size flags */
    if (pArena->size & ARENA_FLAG_FREE)
    {
        ERR("Heap %p: bad flags %08x for in-use arena %p\n",
            subheap->heap, pArena->size & ~ARENA_SIZE_MASK, pArena );
        return FALSE;
    }
    /* Check arena size */
    size = pArena->size & ARENA_SIZE_MASK;
    if ((const char *)(pArena + 1) + size > heapEnd ||
        (const char *)(pArena + 1) + size < (const char *)(pArena + 1))
    {
        ERR("Heap %p: bad size %08lx for in-use arena %p\n", subheap->heap, size, pArena );
        return FALSE;
    }
    /* Check next arena PREV_FREE flag */
    if (((const char *)(pArena + 1) + size < heapEnd) &&
        (*(const DWORD *)((const char *)(pArena + 1) + size) & ARENA_FLAG_PREV_FREE))
    {
        ERR("Heap %p: in-use arena %p next block %p has PREV_FREE flag %x\n",
            subheap->heap, pArena, (const char *)(pArena + 1) + size,*(const DWORD *)((const char *)(pArena + 1) + size) );
        return FALSE;
    }
    /* Check prev free arena */
    if (pArena->size & ARENA_FLAG_PREV_FREE)
    {
        const ARENA_FREE *pPrev = *((const ARENA_FREE * const*)pArena - 1);
        /* Check prev pointer */
        if (!HEAP_IsValidArenaPtr( subheap->heap, pPrev ))
        {
            ERR("Heap %p: bad back ptr %p for arena %p\n",
                subheap->heap, pPrev, pArena );
            return FALSE;
        }
        /* Check that prev arena is free */
        if (!(pPrev->size & ARENA_FLAG_FREE) ||
            (pPrev->magic != ARENA_FREE_MAGIC))
        {
            ERR("Heap %p: prev arena %p invalid for in-use %p\n",
                subheap->heap, pPrev, pArena );
            return FALSE;
        }
        /* Check that prev arena is really the previous block */
        if ((const char *)(pPrev + 1) + (pPrev->size & ARENA_SIZE_MASK) != (const char *)pArena)
        {
            ERR("Heap %p: prev arena %p is not prev for in-use %p\n",
                subheap->heap, pPrev, pArena );
            return FALSE;
        }
    }
    /* Check unused size */
    if (pArena->unused_bytes > size)
    {
        ERR("Heap %p: invalid unused size %08x/%08lx\n", subheap->heap, pArena->unused_bytes, size );
        return FALSE;
    }
    /* Check unused bytes */
    if (pArena->magic == ARENA_PENDING_MAGIC)
    {
        const DWORD *ptr = (const DWORD *)(pArena + 1);
        const DWORD *end = (const DWORD *)((const char *)ptr + size);

        while (ptr < end)
        {
            if (*ptr != ARENA_FREE_FILLER)
            {
                ERR("Heap %p: free block %p overwritten at %p by %08x\n",
                    subheap->heap, pArena + 1, ptr, *ptr );
                if (!*ptr) { HEAP_Dump( subheap->heap ); DbgBreakPoint(); }
                return FALSE;
            }
            ptr++;
        }
    }
    else if (flags & HEAP_TAIL_CHECKING_ENABLED)
    {
        const unsigned char *data = (const unsigned char *)(pArena + 1) + size - pArena->unused_bytes;

        for (i = 0; i < pArena->unused_bytes; i++)
        {
            if (data[i] == ARENA_TAIL_FILLER) continue;
            ERR("Heap %p: block %p tail overwritten at %p (byte %u/%u == 0x%02x)\n",
                subheap->heap, pArena + 1, data + i, i, pArena->unused_bytes, data[i] );
            return FALSE;
        }
    }
    return TRUE;
}


/***********************************************************************
 *           HEAP_IsRealArena  [Internal]
 * Validates a block is a valid arena.
 *
 * RETURNS
 *	TRUE: Success
 *	FALSE: Failure
 */
static BOOL HEAP_IsRealArena( HEAP *heapPtr,   /* [in] ptr to the heap */
              DWORD flags,   /* [in] Bit flags that control access during operation */
              LPCVOID block, /* [in] Optional pointer to memory block to validate */
              BOOL quiet )   /* [in] Flag - if true, HEAP_ValidateInUseArena
                              *             does not complain    */
{
    SUBHEAP *subheap;
    BOOL ret = TRUE;
    const ARENA_LARGE *large_arena;

    flags &= HEAP_NO_SERIALIZE;
    flags |= heapPtr->flags;
    /* calling HeapLock may result in infinite recursion, so do the critsect directly */
    if (!(flags & HEAP_NO_SERIALIZE))
        RtlEnterCriticalSection( &heapPtr->critSection );

    if (block)  /* only check this single memory block */
    {
        const ARENA_INUSE *arena = (const ARENA_INUSE *)block - 1;

        if (!(subheap = HEAP_FindSubHeap( heapPtr, arena )) ||
            ((const char *)arena < (char *)subheap->base + subheap->headerSize))
        {
            if (!(large_arena = find_large_block( heapPtr, block )))
            {
                if (quiet == NOISY)
                    ERR("Heap %p: block %p is not inside heap\n", heapPtr, block );
                else if (WARN_ON(heap))
                    WARN("Heap %p: block %p is not inside heap\n", heapPtr, block );
                ret = FALSE;
            }
            else
                ret = validate_large_arena( heapPtr, large_arena, quiet );
        } else
            ret = HEAP_ValidateInUseArena( subheap, arena, quiet );

        if (!(flags & HEAP_NO_SERIALIZE))
            RtlLeaveCriticalSection( &heapPtr->critSection );
        return ret;
    }

    LIST_FOR_EACH_ENTRY( subheap, &heapPtr->subheap_list, SUBHEAP, entry )
    {
        char *ptr = (char *)subheap->base + subheap->headerSize;
        while (ptr < (char *)subheap->base + subheap->size)
        {
            if (*(DWORD *)ptr & ARENA_FLAG_FREE)
            {
                if (!HEAP_ValidateFreeArena( subheap, (ARENA_FREE *)ptr )) {
                    ret = FALSE;
                    break;
                }
                ptr += sizeof(ARENA_FREE) + (*(DWORD *)ptr & ARENA_SIZE_MASK);
            }
            else
            {
                if (!HEAP_ValidateInUseArena( subheap, (ARENA_INUSE *)ptr, NOISY )) {
                    ret = FALSE;
                    break;
                }
                ptr += sizeof(ARENA_INUSE) + (*(DWORD *)ptr & ARENA_SIZE_MASK);
            }
        }
        if (!ret) break;
    }

    LIST_FOR_EACH_ENTRY( large_arena, &heapPtr->large_list, ARENA_LARGE, entry )
        if (!(ret = validate_large_arena( heapPtr, large_arena, quiet ))) break;

    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    return ret;
}


/***********************************************************************
 *           validate_block_pointer
 *
 * Minimum validation needed to catch bad parameters in heap functions.
 */
static BOOL validate_block_pointer( HEAP *heap, SUBHEAP **ret_subheap, const ARENA_INUSE *arena )
{
    SUBHEAP *subheap;
    BOOL ret = FALSE;

    if (!(*ret_subheap = subheap = HEAP_FindSubHeap( heap, arena )))
    {
        ARENA_LARGE *large_arena = find_large_block( heap, arena + 1 );

        if (!large_arena)
        {
            WARN( "Heap %p: pointer %p is not inside heap\n", heap, arena + 1 );
            return FALSE;
        }
        if ((heap->flags & HEAP_VALIDATE) && !validate_large_arena( heap, large_arena, QUIET ))
            return FALSE;
        return TRUE;
    }

    if ((const char *)arena < (char *)subheap->base + subheap->headerSize)
        WARN( "Heap %p: pointer %p is inside subheap %p header\n", subheap->heap, arena + 1, subheap );
    else if (subheap->heap->flags & HEAP_VALIDATE)  /* do the full validation */
        ret = HEAP_ValidateInUseArena( subheap, arena, QUIET );
    else if ((ULONG_PTR)arena % ALIGNMENT != ARENA_OFFSET)
        WARN( "Heap %p: unaligned arena pointer %p\n", subheap->heap, arena );
    else if (arena->magic == ARENA_PENDING_MAGIC)
        WARN( "Heap %p: block %p used after free\n", subheap->heap, arena + 1 );
    else if (arena->magic != ARENA_INUSE_MAGIC)
        WARN( "Heap %p: invalid in-use arena magic %08x for %p\n", subheap->heap, arena->magic, arena );
    else if (arena->size & ARENA_FLAG_FREE)
        ERR( "Heap %p: bad flags %08x for in-use arena %p\n",
             subheap->heap, arena->size & ~ARENA_SIZE_MASK, arena );
    else if ((const char *)(arena + 1) + (arena->size & ARENA_SIZE_MASK) > (const char *)subheap->base + subheap->size ||
             (const char *)(arena + 1) + (arena->size & ARENA_SIZE_MASK) < (const char *)(arena + 1))
        ERR( "Heap %p: bad size %08x for in-use arena %p\n",
             subheap->heap, arena->size & ARENA_SIZE_MASK, arena );
    else
        ret = TRUE;

    return ret;
}


/***********************************************************************
 *           heap_set_debug_flags
 */
void heap_set_debug_flags( HANDLE handle )
{
    HEAP *heap = HEAP_GetPtr( handle );
    ULONG global_flags = RtlGetNtGlobalFlags();
    ULONG flags = 0;

    if (TRACE_ON(heap)) global_flags |= FLG_HEAP_VALIDATE_ALL;
    if (WARN_ON(heap)) global_flags |= FLG_HEAP_VALIDATE_PARAMETERS;

    if (global_flags & FLG_HEAP_ENABLE_TAIL_CHECK) flags |= HEAP_TAIL_CHECKING_ENABLED;
    if (global_flags & FLG_HEAP_ENABLE_FREE_CHECK) flags |= HEAP_FREE_CHECKING_ENABLED;
    if (global_flags & FLG_HEAP_DISABLE_COALESCING) flags |= HEAP_DISABLE_COALESCE_ON_FREE;
    if (global_flags & FLG_HEAP_PAGE_ALLOCS) flags |= HEAP_PAGE_ALLOCS | HEAP_GROWABLE;

    if (global_flags & FLG_HEAP_VALIDATE_PARAMETERS)
        flags |= HEAP_VALIDATE | HEAP_VALIDATE_PARAMS |
                 HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED;
    if (global_flags & FLG_HEAP_VALIDATE_ALL)
        flags |= HEAP_VALIDATE | HEAP_VALIDATE_ALL |
                 HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED;

    if (RUNNING_ON_VALGRIND) flags = 0; /* no sense in validating since Valgrind catches accesses */

    heap->flags |= flags;
    heap->force_flags |= flags & ~(HEAP_VALIDATE | HEAP_DISABLE_COALESCE_ON_FREE);

    if (flags & (HEAP_FREE_CHECKING_ENABLED | HEAP_TAIL_CHECKING_ENABLED))  /* fix existing blocks */
    {
        SUBHEAP *subheap;
        ARENA_LARGE *large;

        LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry )
        {
            char *ptr = (char *)subheap->base + subheap->headerSize;
            char *end = (char *)subheap->base + subheap->commitSize;
            while (ptr < end)
            {
                ARENA_INUSE *arena = (ARENA_INUSE *)ptr;
                SIZE_T size = arena->size & ARENA_SIZE_MASK;
                if (arena->size & ARENA_FLAG_FREE)
                {
                    SIZE_T count = size;

                    ptr += sizeof(ARENA_FREE) + size;
                    if (ptr >= end) count = end - (char *)((ARENA_FREE *)arena + 1);
                    else count -= sizeof(ARENA_FREE *);
                    mark_block_free( (ARENA_FREE *)arena + 1, count, flags );
                }
                else
                {
                    if (arena->magic == ARENA_PENDING_MAGIC)
                        mark_block_free( arena + 1, size, flags );
                    else
                        mark_block_tail( (char *)(arena + 1) + size - arena->unused_bytes,
                                         arena->unused_bytes, flags );
                    ptr += sizeof(ARENA_INUSE) + size;
                }
            }
        }

        LIST_FOR_EACH_ENTRY( large, &heap->large_list, ARENA_LARGE, entry )
            mark_block_tail( (char *)(large + 1) + large->data_size,
                             large->block_size - sizeof(*large) - large->data_size, flags );
    }

    if ((heap->flags & HEAP_GROWABLE) && !heap->pending_free &&
        ((flags & HEAP_FREE_CHECKING_ENABLED) || RUNNING_ON_VALGRIND))
    {
        void *ptr = NULL;
        SIZE_T size = MAX_FREE_PENDING * sizeof(*heap->pending_free);

        if (!virtual_alloc_aligned( &ptr, 0, &size, MEM_COMMIT, PAGE_READWRITE, 4 ))
        {
            heap->pending_free = ptr;
            heap->pending_pos = 0;
        }
    }
}


/***********************************************************************
 *           RtlCreateHeap   (NTDLL.@)
 *
 * Create a new Heap.
 *
 * PARAMS
 *  flags      [I] HEAP_ flags from "winnt.h"
 *  addr       [I] Desired base address
 *  totalSize  [I] Total size of the heap, or 0 for a growable heap
 *  commitSize [I] Amount of heap space to commit
 *  unknown    [I] Not yet understood
 *  definition [I] Heap definition
 *
 * RETURNS
 *  Success: A HANDLE to the newly created heap.
 *  Failure: a NULL HANDLE.
 */
HANDLE WINAPI RtlCreateHeap( ULONG flags, PVOID addr, SIZE_T totalSize, SIZE_T commitSize,
                             PVOID unknown, PRTL_HEAP_DEFINITION definition )
{
    SUBHEAP *subheap;

    /* Allocate the heap block */

    if (!totalSize)
    {
        totalSize = HEAP_DEF_SIZE;
        flags |= HEAP_GROWABLE;
    }

    if (!(subheap = HEAP_CreateSubHeap( NULL, addr, flags, commitSize, totalSize ))) return 0;

    heap_set_debug_flags( subheap->heap );

    /* link it into the per-process heap list */
    if (processHeap)
    {
        HEAP *heapPtr = subheap->heap;
        RtlEnterCriticalSection( &processHeap->critSection );
        list_add_head( &processHeap->entry, &heapPtr->entry );
        RtlLeaveCriticalSection( &processHeap->critSection );
    }
    else if (!addr)
    {
        processHeap = subheap->heap;  /* assume the first heap we create is the process main heap */
        list_init( &processHeap->entry );
    }

    lh_heap_create(subheap->heap); /* always initialize */
    return subheap->heap;
}


/***********************************************************************
 *           RtlDestroyHeap   (NTDLL.@)
 *
 * Destroy a Heap created with RtlCreateHeap().
 *
 * PARAMS
 *  heap [I] Heap to destroy.
 *
 * RETURNS
 *  Success: A NULL HANDLE, if heap is NULL or it was destroyed
 *  Failure: The Heap handle, if heap is the process heap.
 */
HANDLE WINAPI RtlDestroyHeap( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    SUBHEAP *subheap, *next;
    ARENA_LARGE *arena, *arena_next;
    SIZE_T size;
    void *addr;

    TRACE("%p\n", heap );
    if (!heapPtr) return heap;

    if (heap == processHeap) return heap; /* cannot delete the main process heap */

    if (lh_enabled)
        lh_heap_destroy(heap);

    /* remove it from the per-process list */
    RtlEnterCriticalSection( &processHeap->critSection );
    list_remove( &heapPtr->entry );
    RtlLeaveCriticalSection( &processHeap->critSection );

    heapPtr->critSection.DebugInfo->Spare[0] = 0;
    RtlDeleteCriticalSection( &heapPtr->critSection );

    LIST_FOR_EACH_ENTRY_SAFE( arena, arena_next, &heapPtr->large_list, ARENA_LARGE, entry )
    {
        list_remove( &arena->entry );
        size = 0;
        addr = arena;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }
    LIST_FOR_EACH_ENTRY_SAFE( subheap, next, &heapPtr->subheap_list, SUBHEAP, entry )
    {
        if (subheap == &heapPtr->subheap) continue;  /* do this one last */
        subheap_notify_free_all(subheap);
        list_remove( &subheap->entry );
        size = 0;
        addr = subheap->base;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }
    subheap_notify_free_all(&heapPtr->subheap);
    if (heapPtr->pending_free)
    {
        size = 0;
        addr = heapPtr->pending_free;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }
    size = 0;
    addr = heapPtr->subheap.base;
    NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    return 0;
}


/***********************************************************************
 *           RtlAllocateHeap   (NTDLL.@)
 *
 * Allocate a memory block from a Heap.
 *
 * PARAMS
 *  heap  [I] Heap to allocate block from
 *  flags [I] HEAP_ flags from "winnt.h"
 *  size  [I] Size of the memory block to allocate
 *
 * RETURNS
 *  Success: A pointer to the newly allocated block
 *  Failure: NULL.
 *
 * NOTES
 *  This call does not SetLastError().
 */
void * WINAPI DECLSPEC_HOTPATCH RtlAllocateHeap( HANDLE heap, ULONG flags, SIZE_T size )
{
    ARENA_FREE *pArena;
    ARENA_INUSE *pInUse;
    SUBHEAP *subheap;
    HEAP *heapPtr = HEAP_GetPtr( heap );
    SIZE_T rounded_size;

    /* Validate the parameters */

    if (!heapPtr) return NULL;
    flags &= HEAP_GENERATE_EXCEPTIONS | HEAP_NO_SERIALIZE | HEAP_ZERO_MEMORY;
    flags |= heapPtr->flags;
    rounded_size = ROUND_SIZE(size) + HEAP_TAIL_EXTRA_SIZE( flags );
    if (rounded_size < size)  /* overflow */
    {
        if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus( STATUS_NO_MEMORY );
        return NULL;
    }
    if (rounded_size < HEAP_MIN_DATA_SIZE) rounded_size = HEAP_MIN_DATA_SIZE;

    if (!(flags & HEAP_NO_SERIALIZE)) RtlEnterCriticalSection( &heapPtr->critSection );

    if (rounded_size >= HEAP_MIN_LARGE_BLOCK_SIZE && (flags & HEAP_GROWABLE))
    {
        void *ret = allocate_large_block( heap, flags, size );
        if (ret && lh_enabled) lh_stack_alloc( heap, ret, size );
        if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
        if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );
        TRACE("(%p,%08x,%08lx): returning %p\n", heap, flags, size, ret );
        return ret;
    }

    /* Locate a suitable free block */

    if (!(pArena = HEAP_FindFreeBlock( heapPtr, rounded_size, &subheap )))
    {
        TRACE("(%p,%08x,%08lx): returning NULL\n",
                  heap, flags, size  );
        if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
        if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus( STATUS_NO_MEMORY );
        return NULL;
    }

    /* Remove the arena from the free list */

    list_remove( &pArena->entry );

    /* Build the in-use arena */

    pInUse = (ARENA_INUSE *)pArena;

    /* in-use arena is smaller than free arena,
     * so we have to add the difference to the size */
    pInUse->size  = (pInUse->size & ~ARENA_FLAG_FREE) + sizeof(ARENA_FREE) - sizeof(ARENA_INUSE);
    pInUse->magic = ARENA_INUSE_MAGIC;

    /* Shrink the block */

    HEAP_ShrinkBlock( subheap, pInUse, rounded_size );
    pInUse->unused_bytes = (pInUse->size & ARENA_SIZE_MASK) - size;

    notify_alloc( pInUse + 1, size, flags & HEAP_ZERO_MEMORY );
    initialize_block( pInUse + 1, size, pInUse->unused_bytes, flags );

    if (lh_enabled) lh_stack_alloc( heap, pInUse + 1, size );
    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );

    TRACE("(%p,%08x,%08lx): returning %p\n", heap, flags, size, pInUse + 1 );
    return pInUse + 1;
}


/***********************************************************************
 *           RtlFreeHeap   (NTDLL.@)
 *
 * Free a memory block allocated with RtlAllocateHeap().
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *  ptr   [I] Block to free
 *
 * RETURNS
 *  Success: TRUE, if ptr is NULL or was freed successfully.
 *  Failure: FALSE.
 */
BOOLEAN WINAPI DECLSPEC_HOTPATCH RtlFreeHeap( HANDLE heap, ULONG flags, void *ptr )
{
    ARENA_INUSE *pInUse;
    SUBHEAP *subheap;
    HEAP *heapPtr;

    /* Validate the parameters */

    if (!ptr) return TRUE;  /* freeing a NULL ptr isn't an error in Win2k */

    heapPtr = HEAP_GetPtr( heap );
    if (!heapPtr)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_HANDLE );
        return FALSE;
    }

    flags &= HEAP_NO_SERIALIZE;
    flags |= heapPtr->flags;
    if (!(flags & HEAP_NO_SERIALIZE)) RtlEnterCriticalSection( &heapPtr->critSection );

    /* Inform valgrind we are trying to free memory, so it can throw up an error message */
    notify_free( ptr );

    if (lh_enabled)
        lh_stack_free( heapPtr, ptr );

    /* Some sanity checks */
    pInUse  = (ARENA_INUSE *)ptr - 1;
    if (!validate_block_pointer( heapPtr, &subheap, pInUse )) goto error;

    if (!subheap)
        free_large_block( heapPtr, flags, ptr );
    else
        HEAP_MakeInUseBlockFree( subheap, pInUse );

    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    TRACE("(%p,%08x,%p): returning TRUE\n", heap, flags, ptr );
    return TRUE;

error:
    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_PARAMETER );
    TRACE("(%p,%08x,%p): returning FALSE\n", heap, flags, ptr );
    return FALSE;
}


/***********************************************************************
 *           RtlReAllocateHeap   (NTDLL.@)
 *
 * Change the size of a memory block allocated with RtlAllocateHeap().
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *  ptr   [I] Block to resize
 *  size  [I] Size of the memory block to allocate
 *
 * RETURNS
 *  Success: A pointer to the resized block (which may be different).
 *  Failure: NULL.
 */
PVOID WINAPI RtlReAllocateHeap( HANDLE heap, ULONG flags, PVOID ptr, SIZE_T size )
{
    ARENA_INUSE *pArena;
    HEAP *heapPtr;
    SUBHEAP *subheap;
    SIZE_T oldBlockSize, oldActualSize, rounded_size;
    void *ret;

    if (!ptr) return NULL;
    if (!(heapPtr = HEAP_GetPtr( heap )))
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_HANDLE );
        return NULL;
    }

    /* Validate the parameters */

    flags &= HEAP_GENERATE_EXCEPTIONS | HEAP_NO_SERIALIZE | HEAP_ZERO_MEMORY |
             HEAP_REALLOC_IN_PLACE_ONLY;
    flags |= heapPtr->flags;
    if (!(flags & HEAP_NO_SERIALIZE)) RtlEnterCriticalSection( &heapPtr->critSection );

    rounded_size = ROUND_SIZE(size) + HEAP_TAIL_EXTRA_SIZE(flags);
    if (rounded_size < size) goto oom;  /* overflow */
    if (rounded_size < HEAP_MIN_DATA_SIZE) rounded_size = HEAP_MIN_DATA_SIZE;

    pArena = (ARENA_INUSE *)ptr - 1;
    if (!validate_block_pointer( heapPtr, &subheap, pArena )) goto error;
    if (!subheap)
    {
        if (!(ret = realloc_large_block( heapPtr, flags, ptr, size ))) goto oom;
        goto done;
    }

    /* Check if we need to grow the block */

    oldBlockSize = (pArena->size & ARENA_SIZE_MASK);
    oldActualSize = (pArena->size & ARENA_SIZE_MASK) - pArena->unused_bytes;
    if (rounded_size > oldBlockSize)
    {
        char *pNext = (char *)(pArena + 1) + oldBlockSize;

        if (rounded_size >= HEAP_MIN_LARGE_BLOCK_SIZE && (flags & HEAP_GROWABLE))
        {
            if (flags & HEAP_REALLOC_IN_PLACE_ONLY) goto oom;
            if (!(ret = allocate_large_block( heapPtr, flags, size ))) goto oom;
            memcpy( ret, pArena + 1, oldActualSize );
            notify_free( pArena + 1 );
            HEAP_MakeInUseBlockFree( subheap, pArena );
            goto done;
        }
        if ((pNext < (char *)subheap->base + subheap->size) &&
            (*(DWORD *)pNext & ARENA_FLAG_FREE) &&
            (oldBlockSize + (*(DWORD *)pNext & ARENA_SIZE_MASK) + sizeof(ARENA_FREE) >= rounded_size))
        {
            /* The next block is free and large enough */
            ARENA_FREE *pFree = (ARENA_FREE *)pNext;
            list_remove( &pFree->entry );
            pArena->size += (pFree->size & ARENA_SIZE_MASK) + sizeof(*pFree);
            if (!HEAP_Commit( subheap, pArena, rounded_size )) goto oom;
            notify_realloc( pArena + 1, oldActualSize, size );
            HEAP_ShrinkBlock( subheap, pArena, rounded_size );
        }
        else  /* Do it the hard way */
        {
            ARENA_FREE *pNew;
            ARENA_INUSE *pInUse;
            SUBHEAP *newsubheap;

            if ((flags & HEAP_REALLOC_IN_PLACE_ONLY) ||
                !(pNew = HEAP_FindFreeBlock( heapPtr, rounded_size, &newsubheap )))
                goto oom;

            /* Build the in-use arena */

            list_remove( &pNew->entry );
            pInUse = (ARENA_INUSE *)pNew;
            pInUse->size = (pInUse->size & ~ARENA_FLAG_FREE)
                           + sizeof(ARENA_FREE) - sizeof(ARENA_INUSE);
            pInUse->magic = ARENA_INUSE_MAGIC;
            HEAP_ShrinkBlock( newsubheap, pInUse, rounded_size );

            mark_block_initialized( pInUse + 1, oldActualSize );
            notify_alloc( pInUse + 1, size, FALSE );
            memcpy( pInUse + 1, pArena + 1, oldActualSize );

            /* Free the previous block */

            notify_free( pArena + 1 );
            HEAP_MakeInUseBlockFree( subheap, pArena );
            subheap = newsubheap;
            pArena  = pInUse;
        }
    }
    else
    {
        notify_realloc( pArena + 1, oldActualSize, size );
        HEAP_ShrinkBlock( subheap, pArena, rounded_size );
    }

    pArena->unused_bytes = (pArena->size & ARENA_SIZE_MASK) - size;

    /* Clear the extra bytes if needed */

    if (size > oldActualSize)
        initialize_block( (char *)(pArena + 1) + oldActualSize, size - oldActualSize,
                          pArena->unused_bytes, flags );
    else
        mark_block_tail( (char *)(pArena + 1) + size, pArena->unused_bytes, flags );

    /* Return the new arena */

    ret = pArena + 1;
done:
    if (lh_enabled)
    {
        lh_stack_free(heap, ptr);
        lh_stack_alloc(heap, ret, size);
    }
    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    TRACE("(%p,%08x,%p,%08lx): returning %p\n", heap, flags, ptr, size, ret );
    return ret;

oom:
    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus( STATUS_NO_MEMORY );
    RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_NO_MEMORY );
    TRACE("(%p,%08x,%p,%08lx): returning NULL\n", heap, flags, ptr, size );
    return NULL;

error:
    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_PARAMETER );
    TRACE("(%p,%08x,%p,%08lx): returning NULL\n", heap, flags, ptr, size );
    return NULL;
}


/***********************************************************************
 *           RtlCompactHeap   (NTDLL.@)
 *
 * Compact the free space in a Heap.
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *
 * RETURNS
 *  The number of bytes compacted.
 *
 * NOTES
 *  This function is a harmless stub.
 */
ULONG WINAPI RtlCompactHeap( HANDLE heap, ULONG flags )
{
    static BOOL reported;
    if (!reported++) FIXME( "(%p, 0x%x) stub\n", heap, flags );
    return 0;
}


/***********************************************************************
 *           RtlLockHeap   (NTDLL.@)
 *
 * Lock a Heap.
 *
 * PARAMS
 *  heap  [I] Heap to lock
 *
 * RETURNS
 *  Success: TRUE. The Heap is locked.
 *  Failure: FALSE, if heap is invalid.
 */
BOOLEAN WINAPI RtlLockHeap( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    RtlEnterCriticalSection( &heapPtr->critSection );
    return TRUE;
}


/***********************************************************************
 *           RtlUnlockHeap   (NTDLL.@)
 *
 * Unlock a Heap.
 *
 * PARAMS
 *  heap  [I] Heap to unlock
 *
 * RETURNS
 *  Success: TRUE. The Heap is unlocked.
 *  Failure: FALSE, if heap is invalid.
 */
BOOLEAN WINAPI RtlUnlockHeap( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    RtlLeaveCriticalSection( &heapPtr->critSection );
    return TRUE;
}


/***********************************************************************
 *           RtlSizeHeap   (NTDLL.@)
 *
 * Get the actual size of a memory block allocated from a Heap.
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *  ptr   [I] Block to get the size of
 *
 * RETURNS
 *  Success: The size of the block.
 *  Failure: -1, heap or ptr are invalid.
 *
 * NOTES
 *  The size may be bigger than what was passed to RtlAllocateHeap().
 */
SIZE_T WINAPI RtlSizeHeap( HANDLE heap, ULONG flags, const void *ptr )
{
    SIZE_T ret;
    const ARENA_INUSE *pArena;
    SUBHEAP *subheap;
    HEAP *heapPtr = HEAP_GetPtr( heap );

    if (!heapPtr)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_HANDLE );
        return ~0UL;
    }
    flags &= HEAP_NO_SERIALIZE;
    flags |= heapPtr->flags;
    if (!(flags & HEAP_NO_SERIALIZE)) RtlEnterCriticalSection( &heapPtr->critSection );

    pArena = (const ARENA_INUSE *)ptr - 1;
    if (!validate_block_pointer( heapPtr, &subheap, pArena ))
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_PARAMETER );
        ret = ~0UL;
    }
    else if (!subheap)
    {
        const ARENA_LARGE *large_arena = (const ARENA_LARGE *)ptr - 1;
        ret = large_arena->data_size;
    }
    else
    {
        ret = (pArena->size & ARENA_SIZE_MASK) - pArena->unused_bytes;
    }
    if (!(flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );

    TRACE("(%p,%08x,%p): returning %08lx\n", heap, flags, ptr, ret );
    return ret;
}


/***********************************************************************
 *           RtlValidateHeap   (NTDLL.@)
 *
 * Determine if a block is a valid allocation from a heap.
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *  ptr   [I] Block to check
 *
 * RETURNS
 *  Success: TRUE. The block was allocated from heap.
 *  Failure: FALSE, if heap is invalid or ptr was not allocated from it.
 */
BOOLEAN WINAPI RtlValidateHeap( HANDLE heap, ULONG flags, LPCVOID ptr )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    return HEAP_IsRealArena( heapPtr, flags, ptr, QUIET );
}


/***********************************************************************
 *           RtlWalkHeap    (NTDLL.@)
 *
 * FIXME
 *  The PROCESS_HEAP_ENTRY flag values seem different between this
 *  function and HeapWalk(). To be checked.
 */
NTSTATUS WINAPI RtlWalkHeap( HANDLE heap, PVOID entry_ptr )
{
    LPPROCESS_HEAP_ENTRY entry = entry_ptr; /* FIXME */
    HEAP *heapPtr = HEAP_GetPtr(heap);
    SUBHEAP *sub, *currentheap = NULL;
    NTSTATUS ret;
    char *ptr;
    int region_index = 0;

    if (!heapPtr || !entry) return STATUS_INVALID_PARAMETER;

    if (!(heapPtr->flags & HEAP_NO_SERIALIZE)) RtlEnterCriticalSection( &heapPtr->critSection );

    /* FIXME: enumerate large blocks too */

    /* set ptr to the next arena to be examined */

    if (!entry->lpData) /* first call (init) ? */
    {
        TRACE("begin walking of heap %p.\n", heap);
        currentheap = &heapPtr->subheap;
        ptr = (char*)currentheap->base + currentheap->headerSize;
    }
    else
    {
        ptr = entry->lpData;
        LIST_FOR_EACH_ENTRY( sub, &heapPtr->subheap_list, SUBHEAP, entry )
        {
            if ((ptr >= (char *)sub->base) &&
                (ptr < (char *)sub->base + sub->size))
            {
                currentheap = sub;
                break;
            }
            region_index++;
        }
        if (currentheap == NULL)
        {
            ERR("no matching subheap found, shouldn't happen !\n");
            ret = STATUS_NO_MORE_ENTRIES;
            goto HW_end;
        }

        if (((ARENA_INUSE *)ptr - 1)->magic == ARENA_INUSE_MAGIC ||
            ((ARENA_INUSE *)ptr - 1)->magic == ARENA_PENDING_MAGIC)
        {
            ARENA_INUSE *pArena = (ARENA_INUSE *)ptr - 1;
            ptr += pArena->size & ARENA_SIZE_MASK;
        }
        else if (((ARENA_FREE *)ptr - 1)->magic == ARENA_FREE_MAGIC)
        {
            ARENA_FREE *pArena = (ARENA_FREE *)ptr - 1;
            ptr += pArena->size & ARENA_SIZE_MASK;
        }
        else
            ptr += entry->cbData; /* point to next arena */

        if (ptr > (char *)currentheap->base + currentheap->size - 1)
        {   /* proceed with next subheap */
            struct list *next = list_next( &heapPtr->subheap_list, &currentheap->entry );
            if (!next)
            {  /* successfully finished */
                TRACE("end reached.\n");
                ret = STATUS_NO_MORE_ENTRIES;
                goto HW_end;
            }
            currentheap = LIST_ENTRY( next, SUBHEAP, entry );
            ptr = (char *)currentheap->base + currentheap->headerSize;
        }
    }

    entry->wFlags = 0;
    if (*(DWORD *)ptr & ARENA_FLAG_FREE)
    {
        ARENA_FREE *pArena = (ARENA_FREE *)ptr;

        /*TRACE("free, magic: %04x\n", pArena->magic);*/

        entry->lpData = pArena + 1;
        entry->cbData = pArena->size & ARENA_SIZE_MASK;
        entry->cbOverhead = sizeof(ARENA_FREE);
        entry->wFlags = PROCESS_HEAP_UNCOMMITTED_RANGE;
    }
    else
    {
        ARENA_INUSE *pArena = (ARENA_INUSE *)ptr;

        /*TRACE("busy, magic: %04x\n", pArena->magic);*/

        entry->lpData = pArena + 1;
        entry->cbData = pArena->size & ARENA_SIZE_MASK;
        entry->cbOverhead = sizeof(ARENA_INUSE);
        entry->wFlags = (pArena->magic == ARENA_PENDING_MAGIC) ?
                        PROCESS_HEAP_UNCOMMITTED_RANGE : PROCESS_HEAP_ENTRY_BUSY;
        /* FIXME: can't handle PROCESS_HEAP_ENTRY_MOVEABLE
        and PROCESS_HEAP_ENTRY_DDESHARE yet */
    }

    entry->iRegionIndex = region_index;

    /* first element of heap ? */
    if (ptr == (char *)currentheap->base + currentheap->headerSize)
    {
        entry->wFlags |= PROCESS_HEAP_REGION;
        entry->u.Region.dwCommittedSize = currentheap->commitSize;
        entry->u.Region.dwUnCommittedSize =
                currentheap->size - currentheap->commitSize;
        entry->u.Region.lpFirstBlock = /* first valid block */
                (char *)currentheap->base + currentheap->headerSize;
        entry->u.Region.lpLastBlock  = /* first invalid block */
                (char *)currentheap->base + currentheap->size;
    }
    ret = STATUS_SUCCESS;
    if (TRACE_ON(heap)) HEAP_DumpEntry(entry);

HW_end:
    if (!(heapPtr->flags & HEAP_NO_SERIALIZE)) RtlLeaveCriticalSection( &heapPtr->critSection );
    return ret;
}


/***********************************************************************
 *           RtlGetProcessHeaps    (NTDLL.@)
 *
 * Get the Heaps belonging to the current process.
 *
 * PARAMS
 *  count [I] size of heaps
 *  heaps [O] Destination array for heap HANDLE's
 *
 * RETURNS
 *  Success: The number of Heaps allocated by the process.
 *  Failure: 0.
 */
ULONG WINAPI RtlGetProcessHeaps( ULONG count, HANDLE *heaps )
{
    ULONG total = 1;  /* main heap */
    struct list *ptr;

    RtlEnterCriticalSection( &processHeap->critSection );
    LIST_FOR_EACH( ptr, &processHeap->entry ) total++;
    if (total <= count)
    {
        *heaps++ = processHeap;
        LIST_FOR_EACH( ptr, &processHeap->entry )
            *heaps++ = LIST_ENTRY( ptr, HEAP, entry );
    }
    RtlLeaveCriticalSection( &processHeap->critSection );
    return total;
}

/***********************************************************************
 *           RtlQueryHeapInformation    (NTDLL.@)
 */
NTSTATUS WINAPI RtlQueryHeapInformation( HANDLE heap, HEAP_INFORMATION_CLASS info_class,
                                         PVOID info, SIZE_T size_in, PSIZE_T size_out)
{
    switch (info_class)
    {
    case HeapCompatibilityInformation:
        if (size_out) *size_out = sizeof(ULONG);

        if (size_in < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *(ULONG *)info = 0; /* standard heap */
        return STATUS_SUCCESS;

    default:
        FIXME("Unknown heap information class %u\n", info_class);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/***********************************************************************
 *           RtlSetHeapInformation    (NTDLL.@)
 */
NTSTATUS WINAPI RtlSetHeapInformation( HANDLE heap, HEAP_INFORMATION_CLASS info_class, PVOID info, SIZE_T size)
{
    FIXME("%p %d %p %ld stub\n", heap, info_class, info, size);
    return STATUS_SUCCESS;
}

/*
 * built-in leak detection
 */
#define HL_MAX_NFRAMES  64
struct lh_stack
{
    struct wine_rb_entry entry;
    struct /* only used when grouping */
    {
        struct wine_rb_entry entry;
        SIZE_T size;
        ULONG count;
    } group;
    void *ptr;
    SIZE_T size;
    ULONG hash;
    BOOL invalid;
    DWORD nframes;
    DWORD_PTR frames[HL_MAX_NFRAMES];
};

struct lh_stats
{
    DWORD  nblocks;     /* total # blocks (large + subheap) */
    DWORD  nlarge;      /* # of large blocks */
    DWORD  nused;       /* # blocks in use (large + subheap) */ /* TODO: just subheap?? */
    SIZE_T total;       /* total memory allocated */
    SIZE_T used;        /* memory actually in use */
    SIZE_T overhead;    /* memory used by block headers */
    SIZE_T unused;      /* memory in block used for alignment */
    SIZE_T largest;     /* largest subheap free block */
    SIZE_T committed;   /* amount of committed memory */ /* TODO: subheap + large? */
};

#define LH_CELL_EMPTY       ' '
#define LH_CELL_PARTIAL     '.'
#define LH_CELL_FULL        'X'
#define LH_CELL_SIZE        4096
#define LH_CELLS_PER_ROW    64
#define LH_SIZE_PER_ROW     (LH_CELL_SIZE * LH_CELLS_PER_ROW)
struct lh_row
{
    SIZE_T cellused;
    SIZE_T cellsize;
    int rowidx;
    char *rowptr;
    char row[LH_CELLS_PER_ROW+1];
};

typedef DWORD (WINAPI *SymSetOptions_t)(DWORD);
typedef BOOL (WINAPI *SymInitialize_t)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *SymCleanup_t)(HANDLE);
typedef BOOL (WINAPI *SymFromAddr_t)(HANDLE, DWORD64, DWORD64*, PSYMBOL_INFO);
typedef BOOL (WINAPI *SymGetLineFromAddr64_t)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
typedef BOOL (WINAPI *SymGetModuleInfo_t)(HANDLE, DWORD, PIMAGEHLP_MODULE);
typedef BOOL (WINAPI *SymGetModuleInfoW64_t)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);

static SymSetOptions_t SymSetOptions_func;
static SymInitialize_t SymInitialize_func;
static SymCleanup_t SymCleanup_func;
static SymFromAddr_t SymFromAddr_func;
static SymGetLineFromAddr64_t SymGetLineFromAddr64_func;
static SymGetModuleInfo_t SymGetModuleInfo_func;
static SymGetModuleInfoW64_t SymGetModuleInfoW64_func;

extern USHORT WINAPI RtlCaptureStackBackTrace(ULONG, ULONG, PVOID *, ULONG *);

static void stack_rb_destroy(struct wine_rb_entry *entry, void *context)
{
    struct lh_stack *stack;

    stack = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, entry);
    free(stack);
}

static int stack_rb_compare(const void *key, const struct wine_rb_entry *entry)
{
    const struct lh_stack *stack;

    stack = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, entry);
    return (DWORD_PTR)key - (DWORD_PTR)stack->ptr;
}

static inline int lh_compare_stacks(const struct lh_stack *a, const struct lh_stack *b)
{
    int diff;
    DWORD i;

    if ((diff = a->hash - b->hash))
        return diff;

    if ((diff = a->nframes - b->nframes))
        return diff;

    for (i = 0; i < a->nframes; i++)
    {
        if ((diff = a->frames[i] - b->frames[i]))
            return diff;
    }

    return 0;
}

static int lh_stack_group_rb_compare(const void *_key, const struct wine_rb_entry *entry)
{
    const struct lh_stack *set;
    const struct lh_stack *key;

    key = _key;
    set = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, group.entry);
    return lh_compare_stacks(key, set);
}

static int lh_stack_group_by_size_rb_compare(const void *_key, const struct wine_rb_entry *entry)
{
    const struct lh_stack *set;
    const struct lh_stack *key;
    int diff;

    key = _key;
    set = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, group.entry);
    if ((diff = key->group.size - set->group.size))
        return diff;
    if ((diff = key->group.count - set->group.count))
        return diff;
    return lh_compare_stacks(key, set);
}

static int lh_stack_group_by_freq_rb_compare(const void *_key, const struct wine_rb_entry *entry)
{
    const struct lh_stack *set;
    const struct lh_stack *key;
    int diff;

    key = _key;
    set = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, group.entry);
    if ((diff = key->group.count - set->group.count))
        return diff;
    if ((diff = key->group.size - set->group.size))
        return diff;
    return lh_compare_stacks(key, set);
}

static HMODULE hdbghelp;
static DWORD_PTR dbghelp_start;
static DWORD_PTR dbghelp_end;

static int lh_skip = 2;
static SIZE_T lh_min;
static SIZE_T lh_max = ~0;
static DWORD lh_nframes = 20;
static DWORD lh_nprint = 32;
static WCHAR lh_include_path[MAX_PATH]; /* NOTE: only support single modules for now */
static DWORD_PTR lh_include_start;      /*       since probably only troubleshoot one */
static DWORD_PTR lh_include_end;        /*       memory leak at a time */
static BOOL lh_dump_each;
static short lh_port_min = 0;
static short lh_port_max = 0;
static BOOL lh_listener_running;

enum lh_sort_type {
    LH_SORT_UNKNOWN = -1,
    LH_SORT_NONE,
    LH_SORT_SIZE,
    LH_SORT_FREQ
};
static enum lh_sort_type lh_sort = LH_SORT_NONE;

static inline NTSTATUS lh_getenv(const WCHAR *env, WCHAR *buffer, DWORD size)
{
    UNICODE_STRING envname;
    UNICODE_STRING envval;
    NTSTATUS status;

    RtlInitUnicodeString(&envname, env);
    envval.Length = 0;
    envval.MaximumLength = size;
    envval.Buffer = buffer;
    if (!(status = RtlQueryEnvironmentVariable_U(NULL, &envname, &envval)))
        buffer[size-1] = 0;
    return status;
}

static NTSTATUS lh_getenv_ul(const WCHAR *env, unsigned long *val)
{
    NTSTATUS status;
    WCHAR value[32];
    int base;

    if ((status = lh_getenv(env, value, sizeof(value))))
        return status;

    base = 10;
    if ((strlenW(value) > 2) &&
        (value[0] == '0') &&
        (value[1] == 'x' || value[1] == 'X'))
        base = 16;
    *val = strtoulW(value, 0, base);
    return STATUS_SUCCESS;
}

static inline BOOL lh_filter_exclude(const struct lh_stack *stack)
{
    DWORD i;

    /* ignore stacks from dbghelp */
    for (i = 0; i < stack->nframes; i++)
    {
        if (stack->frames[i] >= dbghelp_start && stack->frames[i] < dbghelp_end)
            return TRUE;
    }

    return FALSE;
}

static BOOL lh_filter_include(const struct lh_stack *stack)
{
    DWORD i;

    if (!lh_include_start &&
        !lh_fetch_module_limits(lh_include_path, &lh_include_start, &lh_include_end))
        return FALSE;

    for (i = 0; i < stack->nframes; i++)
    {
        if (stack->frames[i] >= lh_include_start && stack->frames[i] < lh_include_end)
            return TRUE;
    }

    return FALSE;
}

typedef void (CALLBACK *LDRENUMPROC)(LDR_MODULE *, void *, BOOLEAN *);
extern NTSTATUS WINAPI LdrEnumerateLoadedModules(void *, LDRENUMPROC, void *);

static RTL_CRITICAL_SECTION lh_modcs;
static RTL_CRITICAL_SECTION_DEBUG lh_modcsdbg =
{
    0, 0, &lh_modcs,
    { &lh_modcsdbg.ProcessLocksList, &lh_modcsdbg.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": lh_modcs") }
};
static RTL_CRITICAL_SECTION lh_modcs = { &lh_modcsdbg, -1, 0, 0, 0, 0 };

static DWORD lh_stack_fetch(struct lh_stack *stack)
{
    DWORD i;

    /* randomly crashes on certain java frames */
    __TRY
    {
        stack->nframes = RtlCaptureStackBackTrace(lh_skip, ARRAY_SIZE(stack->frames),
                                                  (void*)stack->frames, &stack->hash);
    }
    __EXCEPT_PAGE_FAULT
    {
        stack->nframes = 0;
    }
    __ENDTRY

    if (lh_filter_exclude(stack))
        return 0;

    /* trim frames that have zero */
    for (i = 0; i < stack->nframes; i++)
    {
        /* remove frames that may be bogus in linux java */
        if (!stack->frames[i])
            break;
    }
    stack->nframes = i;

    if (stack->nframes)
    {
        i = modules_count_loaded(stack->nframes, (const void **)stack->frames);
        stack->invalid = i != stack->nframes;
        stack->nframes = i;
    }

    if (lh_include_path[0] && !lh_filter_include(stack))
        return 0;

    return stack->nframes;
}

static void lh_stack_print_bytes(const void *ptr, DWORD size)
{
    const BYTE *bytes = ptr;
    const BYTE *end = bytes + size;
    char buffer[128]; /* >= 16 + 2 + 16 * 3 + 1 + 16 + 1 + 1 */
    DWORD remain;     /*    address  bytes    | ascii  |  \0 */
    DWORD line;
    DWORD i, j;

    while (bytes < end)
    {
        line = min(size, 16);
        size -= line;

        j = sprintf(buffer, "%p: ", bytes);
        for (i = 0; i < line; i++)
        {
            j += sprintf(&buffer[j], "%02x ", bytes[i]);
        }

        remain = 16 - line;
        if (remain)
        {
            memset(&buffer[j], ' ', remain * 3); /* '00 ' */
            j += remain * 3;
        }

        buffer[j++] = '|';
        for (i = 0; i < line; i++)
            buffer[j++] = isalnum(bytes[i]) || ispunct(bytes[i]) ? bytes[i] : '.';
        memset(&buffer[j], '.', remain);
        j += remain;
        buffer[j++] = '|';
        buffer[j] = 0;

        MESSAGE("%s\n", buffer);

        bytes += 16;
    }
}

static void lh_stack_print_frames(const HEAP *heap, const struct lh_stack *stack)
{
    BYTE buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buffer;
    IMAGEHLP_MODULEW64 modinfo;
    IMAGEHLP_LINE64 il;
    char numbuf[16];
    DWORD64 disp64;
    DWORD nframes;
    DWORD disp;
    int i;

    nframes = min(stack->nframes, lh_nframes); /* in case given nframes is bogus */
    for (i = 0; i < nframes; i++)
    {
        disp = 0;
        disp64 = 0;
        memset(buffer, 0, sizeof(buffer));
        si->SizeOfStruct = sizeof(*si);
        si->MaxNameLen = MAX_SYM_NAME;
        memset(&il, 0, sizeof(il));
        il.SizeOfStruct = sizeof(il);
        SymFromAddr_func(GetCurrentProcess(), stack->frames[i], &disp64, si);

        numbuf[0] = 0;
        if (SymGetLineFromAddr64_func(GetCurrentProcess(), stack->frames[i], &disp, &il))
            sprintf(numbuf, ":%u", il.LineNumber);

        memset(&modinfo, 0, sizeof(modinfo));
        modinfo.SizeOfStruct = sizeof(modinfo);
        SymGetModuleInfoW64_func(GetCurrentProcess(), stack->frames[i], &modinfo);
        MESSAGE("  [%d] 0x%s+0x%s %s (0x%s %s) %s%s\n", i,
                wine_dbgstr_longlong(stack->frames[i] - disp64),
                wine_dbgstr_longlong(disp64), debugstr_an(si->Name, si->NameLen),
                wine_dbgstr_longlong(modinfo.BaseOfImage), debugstr_w(modinfo.ImageName),
                il.LineNumber ? debugstr_a(il.FileName): "", numbuf);
    }

}

static void lh_stack_print(HEAP *heap, const struct lh_stack *stack)
{
    MESSAGE("0x%s bytes leaked in heap %p at %p:%s\n",
            wine_dbgstr_longlong(stack->size), heap, stack->ptr,
            stack->invalid ? "(invalid)" : "");
    lh_stack_print_bytes(stack->ptr, min(stack->size, lh_nprint));
    lh_stack_print_frames(heap, stack);
}

static void lh_stack_group_print(const HEAP *heap, const struct lh_stack *stack)
{
    MESSAGE("0x%s bytes in %u sets leaked in heap %p:%s\n",
            wine_dbgstr_longlong(stack->group.size), stack->group.count, heap,
            stack->invalid ? "(invalid)" : "");
    lh_stack_print_frames(heap, stack);
}

static void lh_dump_stack_groups(HEAP *heap)
{
    struct lh_stack *stack;
    struct lh_stack *group;
    struct wine_rb_entry *entry;
    struct wine_rb_tree leak_groups;
    struct wine_rb_tree leak_sorted;
    struct wine_rb_tree *leaks;

    wine_rb_init(&leak_groups, lh_stack_group_rb_compare);
    switch (lh_sort)
    {
        case LH_SORT_SIZE:
            wine_rb_init(&leak_sorted, lh_stack_group_by_size_rb_compare);
            leaks = &leak_sorted;
            break;
        case LH_SORT_FREQ:
            wine_rb_init(&leak_sorted, lh_stack_group_by_freq_rb_compare);
            leaks = &leak_sorted;
            break;
        default:
            leaks = &leak_groups;
            break;
    }

    WINE_RB_FOR_EACH_ENTRY(stack, &heap->leaks, struct lh_stack, entry)
    {
        if ((entry = wine_rb_get(&leak_groups, stack)))
        {
            group = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, group.entry);
            group->group.size += stack->size;
            group->group.count++;
        }
        else
        {
            stack->group.size = stack->size;
            stack->group.count = 1;
            wine_rb_put(&leak_groups, stack, &stack->group.entry);
        }
    }

    if (leaks == &leak_sorted)
    {
        while ((entry = wine_rb_head(leak_groups.root)))
        {
            group = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, group.entry);
            wine_rb_remove(&leak_groups, entry);
            wine_rb_put(&leak_sorted, group, &group->group.entry);
        }
    }

    WINE_RB_FOR_EACH_ENTRY(group, leaks, struct lh_stack, group.entry)
    {
        lh_stack_group_print(heap, group);
    }
}

static inline SIZE_T lh_dump_summary_heap(const HEAP *heap, DWORD *count)
{
    struct lh_stack *stack;
    DWORD nstacks;
    SIZE_T sum;

    sum = 0;
    nstacks = 0;
    WINE_RB_FOR_EACH_ENTRY(stack, &heap->leaks, struct lh_stack, entry)
    {
        sum += stack->size;
        nstacks++;
    }
    *count = nstacks;

    return sum;
}

static inline const char *lh_pretty_size(SIZE_T size)
{
    static char buffer[2][32];
    static int idx;

    idx = (idx + 1) % ARRAY_SIZE(buffer);
    if (size >= 1024*1024*1024)
        snprintf(buffer[idx], sizeof(buffer[idx]), "%.3f GB", size / 1024.0 / 1024.0 / 1024.0);
    else if (size >= 1024*1024)
        snprintf(buffer[idx], sizeof(buffer[idx]), "%.3f MB", size / 1024.0 / 1024.0);
    else if (size >= 1024)
        snprintf(buffer[idx], sizeof(buffer[idx]), "%.3f KB", size / 1024.0);
    else
        snprintf(buffer[idx], sizeof(buffer[idx]), "%lu B", size);
    return buffer[idx];
}

static void lh_dump_summary(void)
{
    HEAP *heap;
    SIZE_T sum;
    SIZE_T leaked;
    DWORD count;
    DWORD nstacks;

    sum = 0;
    count = 0;
    LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
    {
        nstacks = 0;
        leaked = lh_dump_summary_heap(heap, &nstacks);
        if (nstacks)
            MESSAGE("Heap %p: 0x%lx bytes (%s) leaked in %u records\n", heap, leaked,
                    lh_pretty_size(leaked), nstacks);
        sum += leaked;
        count += nstacks;
    }
    leaked = lh_dump_summary_heap(processHeap, &nstacks);
    MESSAGE("Main: %p: 0x%lx bytes (%s) leaked in %u records\n", processHeap, leaked,
            lh_pretty_size(leaked), nstacks);
    sum += leaked;
    count += nstacks;
    MESSAGE("Summary: 0x%lx bytes (%s) leaked in %u records\n", sum, lh_pretty_size(sum), count);
}

static inline void lh_dump_heap(HEAP *heap)
{
    struct lh_stack *stack;

    if (!lh_dump_each)
        lh_dump_stack_groups(heap);
    else
    {
        WINE_RB_FOR_EACH_ENTRY(stack, &heap->leaks, struct lh_stack, entry)
        {
            lh_stack_print(heap, stack);
        }
    }
}

static inline BOOL lh_validate_heap(const HEAP *search)
{
    HEAP *heap;

    if (search == processHeap)
        return TRUE;
    LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
    {
        if (heap == search)
            return TRUE;
    }
    return FALSE;
}

static void lh_send(int fd, const char *fmt, ...)
{
    va_list va;
    char buffer[128];

    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    send(fd, buffer, strlen(buffer), MSG_DONTWAIT);
}

static void lh_dump(int connectfd, HEAP *spec)
{
    HEAP *heap;

    /* invade process to get full symbol info for printing stack */
    SymSetOptions_func(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (!SymInitialize_func(GetCurrentProcess(), NULL, TRUE))
    {
        WARN_(heapleaks)("failed to initialize dbghelp\n");
        return;
    }

    if (spec)
    {
        if (lh_validate_heap(spec))
            lh_dump_heap(spec);
        else
            lh_send(connectfd, "invalid heap: %p\n", spec);
    }
    else
    {
        lh_dump_heap(processHeap);
        LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
        {
            lh_dump_heap(heap);
        }

        lh_dump_summary();
    }
    SymCleanup_func(GetCurrentProcess());
}

static void lh_cmd_clear(int connectfd)
{
    HEAP *heap;

    LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
    {
        lh_heap_destroy(heap);
    }
    lh_heap_destroy(processHeap);
    MESSAGE("leaks freed\n");
    lh_send(connectfd, "leaks freed\n");
}

static void lh_cmd_list(int connectfd)
{
    HEAP *heap;

    lh_send(connectfd, "Heaps:\n");
    LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
    {
        lh_send(connectfd, "%p\n", heap);
    }
    lh_send(connectfd, "%p (process heap)\n", processHeap);
}

void lh_stack_alloc(HEAP *heap, void *ptr, SIZE_T size)
{
    struct lh_stack *stack;
    struct lh_stack tmp;

    if (size < lh_min || size > lh_max)
        return;

    /* ideally, the stack information would be in the very block being allocated
       but that seemed to cause problems;  either it was a sloppy first attempt
       or some memory corruption that doesn't usually show */
    tmp.nframes = 0;
    tmp.invalid = FALSE;
    if (!lh_stack_fetch(&tmp))
        return;

    stack = malloc(FIELD_OFFSET(struct lh_stack, frames[tmp.nframes]));
    stack->ptr = ptr;
    stack->size = size;
    stack->hash = tmp.hash;
    stack->invalid = tmp.invalid;
    stack->nframes = tmp.nframes;
    memcpy(stack->frames, tmp.frames, tmp.nframes * sizeof(tmp.frames[0]));
    wine_rb_put(&heap->leaks, ptr, &stack->entry);
}

void lh_stack_free(HEAP *heap, void *ptr)
{
    struct wine_rb_entry *entry;
    struct lh_stack *stack;

    if (!(entry = wine_rb_get(&heap->leaks, ptr)))
        return;

    wine_rb_remove(&heap->leaks, entry);

    stack = WINE_RB_ENTRY_VALUE(entry, struct lh_stack, entry);
    free(stack);
}

static void CALLBACK lh_pin_modules(LDR_MODULE *module, void *context, BOOLEAN *stop)
{
    /* keep dlls loaded in memory so that their addresses are valid
       and consistent till the end when records are dumped */
    LdrAddRefDll(LDR_ADDREF_DLL_PIN, module->BaseAddress);
}

extern NTSTATUS WINAPI LdrRegisterDllNotification(ULONG, PLDR_DLL_NOTIFICATION_FUNCTION,
                                                  void *, void **);
void CALLBACK lh_dll_pin(ULONG reason, LDR_DLL_NOTIFICATION_DATA *data, void *mod)
{
    MESSAGE("%s: [%lx-%lx] %s\n", __FUNCTION__, (DWORD_PTR)data->Loaded.DllBase,
                                  (DWORD_PTR)data->Loaded.DllBase + data->Loaded.SizeOfImage,
                                  debugstr_us(data->Loaded.FullDllName));
    if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED)
    {
        /* keep dlls loaded in memory so that their addresses are valid
           and consistent till the end when records are dumped */
        LdrAddRefDll(LDR_ADDREF_DLL_PIN, data->Loaded.DllBase);
    }
}

static void lh_heap_create(HEAP *heap)
{
    wine_rb_init(&heap->leaks, stack_rb_compare);
}

static void lh_heap_destroy(HEAP *heap)
{
    wine_rb_destroy(&heap->leaks, stack_rb_destroy, NULL);
}

static void lh_lock_all_heaps(void)
{
    HEAP *heap;

    RtlEnterCriticalSection(&processHeap->critSection);
    LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
    {
        RtlEnterCriticalSection(&heap->critSection);
    }
}

static void lh_unlock_all_heaps(void)
{
    HEAP *heap;

    LIST_FOR_EACH_ENTRY_REV(heap, &processHeap->entry, HEAP, entry)
    {
        RtlLeaveCriticalSection(&heap->critSection);
    }
    RtlLeaveCriticalSection(&processHeap->critSection);
}

static void lh_subheap_info(SUBHEAP *subheap, struct lh_stats *stats)
{
    char *ptr;
    SIZE_T free_size = 0;
    SIZE_T used_size = 0;
    SIZE_T unused_size = 0;
    SIZE_T overhead = subheap->headerSize;
    SIZE_T largest = 0;
    SIZE_T size;
    SIZE_T nused;
    SIZE_T ntotal;

    nused = 0;
    ntotal = 0;
    ptr = (char *)subheap->base + subheap->headerSize;
    while (ptr < (char *)subheap->base + subheap->size)
    {
        ntotal++;
        if (*(DWORD *)ptr & ARENA_FLAG_FREE)
        {
            ARENA_FREE *arena = (ARENA_FREE *)ptr;
            size = arena->size & ARENA_SIZE_MASK;
            ptr += sizeof(*arena) + size;
            overhead += sizeof(ARENA_FREE);
            free_size += size;
            largest = max(size, largest);
        }
        else
        {
            ARENA_INUSE *arena = (ARENA_INUSE *)ptr;
            size = arena->size & ARENA_SIZE_MASK;
            ptr += sizeof(*arena) + size;
            overhead += sizeof(ARENA_INUSE);
            used_size += size;
            unused_size += arena->unused_bytes;
            nused++;
        }
    }

    stats->nblocks += ntotal;
    stats->nused += nused;
    stats->total += used_size + free_size;
    stats->used += used_size;
    stats->overhead += overhead;
    stats->unused += unused_size;
    stats->largest = max(largest, stats->largest);
    stats->committed += subheap->commitSize;

    MESSAGE("subheap %p\n", subheap);
    MESSAGE("\ttotal # blocks %lu (used %lu free %lu)\n", ntotal, nused, ntotal - nused);
    MESSAGE("\ttotal size   %08lx (%10s)\n",
            subheap->size, lh_pretty_size(subheap->size));
    MESSAGE("\tcommit       %08lx (%10s) (%5.2f%% of total)\n", subheap->commitSize,
            lh_pretty_size(subheap->commitSize), subheap->commitSize * 100.0 / subheap->size);
    MESSAGE("\tfree size    %08lx (%10s) (%5.2f%% of subheap)\n", free_size,
            lh_pretty_size(free_size), free_size * 100.0 / subheap->size);
    MESSAGE("\tused size    %08lx (%10s) (%5.2f%% of subheap)\n", used_size,
            lh_pretty_size(used_size), used_size * 100.0 / subheap->size);
    MESSAGE("\tunused_size  %08lx (%10s) (%5.2f%% of subheap)\n", unused_size,
            lh_pretty_size(unused_size), unused_size * 100.0 / subheap->size);
    MESSAGE("\toverhead     %08lx (%10s) (%5.2f%% of subheap)\n", overhead,
            lh_pretty_size(overhead), overhead * 100.0 / subheap->size);
    MESSAGE("\tlargest free %08lx (%10s) (%5.2f%% of subheap)\n", largest,
            lh_pretty_size(largest), largest * 100.0 / subheap->size);
    MESSAGE("\tinternal fragmentation %5.2f%%\n",
            unused_size*100.0/used_size);
    MESSAGE("\texternal fragmentation %5.2f%%\n",
            (free_size - largest) * 100.0 / free_size);
}

static void lh_large_blocks_info(HEAP *heap, struct lh_stats *stats)
{
    ARENA_LARGE *arena;
    DWORD nblocks;
    SIZE_T block_size;
    SIZE_T data_size;

    MESSAGE("large blocks for heap %p\n", heap);
    block_size = 0;
    data_size = 0;
    nblocks = 0;
    LIST_FOR_EACH_ENTRY(arena, &heap->large_list, ARENA_LARGE, entry)
    {
        ++nblocks;
        block_size += arena->block_size;
        data_size += arena->data_size;
        MESSAGE("\t%08lx: data %lx (%s) block %lx (%s)\n", (long)(arena+1),
                arena->data_size, lh_pretty_size(arena->data_size),
                arena->block_size, lh_pretty_size(arena->block_size));
        /* TODO: clean up */
        stats->nblocks++;
        stats->nlarge++;
        stats->nused++; /* TODO: subheap only? */
        stats->total += arena->block_size;
        stats->used += arena->data_size;
        stats->overhead += sizeof(*arena);
        stats->unused = arena->block_size - arena->data_size;
        stats->committed += arena->block_size; /* TODO: subheap only? */
    }
    MESSAGE("\ttotal # blocks %u\n", nblocks);
    if (!nblocks) return;
    MESSAGE("\ttotal block size %08lx (%10s)\n", block_size, lh_pretty_size(block_size));
    MESSAGE("\ttotal data size  %08lx (%10s)\n", data_size, lh_pretty_size(data_size));
}

static inline void lh_runner(void)
{
    MESSAGE("============================================================================\n");
}

static void lh_print_stats(const struct lh_stats *stats)
{
    SIZE_T free_size;

    free_size = stats->total - stats->used;
    MESSAGE("\ttotal # blocks  %u (subheap + large)\n", stats->nblocks);
    MESSAGE("\t# large blocks  %u (subheap %u)\n", stats->nlarge,
            stats->nblocks - stats->nlarge);
    MESSAGE("\t# in-use blocks %u (subheap %u)\n", stats->nused,
            stats->nused - stats->nlarge);
    MESSAGE("\ttotal size      %08lx (%10s)\n", stats->total, lh_pretty_size(stats->total));
    MESSAGE("\tin-use size     %08lx (%10s)\n", stats->used, lh_pretty_size(stats->used));
    MESSAGE("\tfree size       %08lx (%10s)\n", free_size, lh_pretty_size(free_size));
    MESSAGE("\toverhead size   %08lx (%10s)\n", stats->overhead,
            lh_pretty_size(stats->overhead));
    MESSAGE("\tlargest free    %08lx (%10s)\n", stats->largest,
            lh_pretty_size(stats->largest));
    MESSAGE("\tcommitted       %08lx (%10s)\n", stats->committed,
            lh_pretty_size(stats->committed));
    /* TODO: internal + external fragmentation */
    lh_runner();
}

static void lh_heap_info(HEAP *heap, struct lh_stats *stats)
{
    SUBHEAP *subheap;
    struct lh_stats heapstats;

    memset(&heapstats, 0, sizeof(heapstats));
    MESSAGE("heap: %p\n", heap);
    MESSAGE("subheaps: %u\n", list_count(&heap->subheap_list));
    LIST_FOR_EACH_ENTRY(subheap, &heap->subheap_list, SUBHEAP, entry)
    {
        lh_subheap_info(subheap, &heapstats);
    }
    lh_large_blocks_info(heap, &heapstats);
    MESSAGE("totals for heap %p\n", heap);
    lh_print_stats(&heapstats);

    stats->nblocks += heapstats.nblocks;
    stats->nlarge += heapstats.nlarge;
    stats->nused += heapstats.nused;
    stats->total += heapstats.total;
    stats->used += heapstats.used;
    stats->overhead += heapstats.overhead;
    stats->unused += heapstats.unused;
    stats->largest = max(stats->largest, heapstats.largest);
    stats->committed += heapstats.committed;
}

static void lh_info(int connectfd, HEAP *heap)
{
    struct lh_stats stats;

    memset(&stats, 0, sizeof(stats));
    lh_lock_all_heaps();
    if (heap)
    {
        if (lh_validate_heap(heap))
            lh_heap_info(heap, &stats);
        else
            lh_send(connectfd, "invalid heap: %p\n", heap);
    }
    else
    {
        LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
        {
            lh_heap_info(heap, &stats);
        }
        lh_heap_info(processHeap, &stats);
        MESSAGE("totals for all heaps\n");
        lh_print_stats(&stats);
    }
    lh_unlock_all_heaps();
}

static void inline lh_row_init(struct lh_row *row, char *ptr)
{
    memset(row, 0, sizeof(*row));
    row->rowptr = ptr;
}

static void lh_row_flush(struct lh_row *row)
{
    DWORD skipped;

    while (row->rowidx < LH_CELLS_PER_ROW)
        row->row[row->rowidx++] = LH_CELL_EMPTY;

    row->row[row->rowidx] = 0;
    MESSAGE("%p: |%s|\n", row->rowptr, row->row);
    row->rowidx = 0;
    row->rowptr += LH_SIZE_PER_ROW;

    /* don't skip partial rows */ /* TODO: only works if large size added */
    if (row->cellused && (row->cellused != row->cellsize))
        return;

    skipped = 0;
    while (row->cellsize > LH_SIZE_PER_ROW)
    {
        row->cellsize -= LH_SIZE_PER_ROW;
        row->rowptr += LH_SIZE_PER_ROW;
        ++skipped;
    }
    if (skipped)
        MESSAGE("  (%u %s rows skipped)\n", skipped, !row->cellused ? "free" : "used");
}

/* TODO: clean this up */
static void lh_row_add(struct lh_row *row, SIZE_T size, char type)
{
    SIZE_T leftused;
    SIZE_T left;

    if (type == LH_CELL_FULL) row->cellused += size;
    row->cellsize += size;

    while (row->cellsize >= LH_CELL_SIZE)
    {
        left = min(row->cellsize, LH_CELL_SIZE);
        leftused = min(row->cellused, LH_CELL_SIZE);

        row->row[row->rowidx++] = row->cellused && (row->cellused != row->cellsize) ?
                                    LH_CELL_PARTIAL : type;
        row->cellsize -= left;
        row->cellused -= leftused;
        if (row->rowidx == LH_CELLS_PER_ROW)
            lh_row_flush(row);
    }
}

static void lh_subheap_map(SUBHEAP *subheap)
{
    char *ptr;
    SIZE_T size;
    struct lh_row row;

    MESSAGE("heap %p subheap %p\n", subheap->heap, subheap);
    lh_row_init(&row, subheap->base);
    lh_row_add(&row, subheap->headerSize, LH_CELL_FULL);
    ptr = (char *)subheap->base + subheap->headerSize;
    while (ptr < (char *)subheap->base + subheap->size)
    {

        if (*(DWORD *)ptr & ARENA_FLAG_FREE)
        {
            ARENA_FREE *arena = (ARENA_FREE *)ptr;
            size = arena->size & ARENA_SIZE_MASK;
            ptr += sizeof(*arena) + size;
            lh_row_add(&row, size, LH_CELL_EMPTY); /* TODO: include decommitted */
        }
        else
        {
            ARENA_INUSE *arena = (ARENA_INUSE *)ptr;
            size = arena->size & ARENA_SIZE_MASK;
            ptr += sizeof(*arena) + size;
            lh_row_add(&row, size, LH_CELL_FULL);
        }
    }
    lh_row_flush(&row);
}


static void lh_heap_map(HEAP *heap)
{
    ARENA_LARGE *arena;
    SUBHEAP *subheap;

    MESSAGE("map for heap %p\n", heap);
    MESSAGE("each cell is 0x%x (%s) (each row is 0x%x (%s))\n",
        LH_CELL_SIZE, lh_pretty_size(LH_CELL_SIZE),
        LH_SIZE_PER_ROW, lh_pretty_size(LH_SIZE_PER_ROW));
    MESSAGE("key: cell in-use: '%c' free: '%c' mix of in-use and free: '%c'\n",
        LH_CELL_FULL, LH_CELL_EMPTY, LH_CELL_PARTIAL);
    LIST_FOR_EACH_ENTRY(subheap, &heap->subheap_list, SUBHEAP, entry)
    {
        lh_subheap_map(subheap);
    }

    LIST_FOR_EACH_ENTRY(arena, &heap->large_list, ARENA_LARGE, entry)
    {
        MESSAGE("large block %p (data %lx (%s) block %lx (%s)) for heap %p\n", arena,
                arena->data_size, lh_pretty_size(arena->data_size),
                arena->block_size, lh_pretty_size(arena->block_size), heap);
    }
    lh_runner();
}

static void lh_map(int connectfd, HEAP *heap)
{
    lh_lock_all_heaps();
    if (heap)
    {
        if (lh_validate_heap(heap))
            lh_heap_map(heap);
        else
            lh_send(connectfd, "invalid heap: %p\n", heap);
    }
    else
    {
        LIST_FOR_EACH_ENTRY(heap, &processHeap->entry, HEAP, entry)
        {
            lh_heap_map(heap);
        }
        lh_heap_map(processHeap);
    }
    lh_unlock_all_heaps();
}

static void lh_thread_help(int fd)
{
    const char help[] =
"dump [sort by] [heap]  dump all leak records by group\n"
" where sort by is one of:\n"
"   size                sort grouped records by size\n"
"   freq                sort grouped records by frequency\n"
"   none                grouped records are unsorted\n"
"   [heap]              use 'summary' to list heaps (ex: dump size 0x10000)\n"
"                       default: HEAPLEAKS_SORTBY or unsorted on all heaps\n"
"clear                  clear all leak records\n"
"summary                print only summary of leaks\n"
"list                   list all heaps\n"
"info [heap]            print information about all or specific heap\n"
"map [heap]             print map of blocks for all or specific heap\n"
"quit                   detach from process\n";
    send(fd, help, sizeof(help), MSG_DONTWAIT);
}

static inline enum lh_sort_type lh_str_to_sort_type(const char *str)
{
    if (!strcasecmp(str, "none"))
        return LH_SORT_NONE;
    else if (!strcasecmp(str, "size"))
        return LH_SORT_SIZE;
    else if (!strcasecmp(str, "freq"))
        return LH_SORT_FREQ;
    return LH_SORT_UNKNOWN;
}

static inline HEAP *lh_parse_heap(const char *token)
{
    if (token[0] != '0' || (token[1] != 'x' && token[1] != 'X'))
        return NULL;

    return (HEAP *)strtol(token, NULL, 16);
}

static HANDLE lh_thread;
static void CALLBACK lh_thread_proc(LPVOID arg)
{
    HANDLE ready = arg;
    HEAP *heap;
    int fd;
    int one;
    int connectfd;
    short port;
    struct sockaddr_in addr;
    char buffer[64];
    char *token, *next;
    ssize_t i, nread;
    enum lh_sort_type sort;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        goto error;

    one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        goto error;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    for (port = lh_port_min; port <= lh_port_max; port++)
    {
        addr.sin_port = htons(port);
        if (!bind(fd, (const struct sockaddr *)&addr, sizeof(addr)))
            break;
    }
    if (port > lh_port_max)
        goto error;

    if (listen(fd, 1) < 0)
        goto error;

    lh_listener_running = TRUE;
    NtSetEvent(ready, NULL);
    MESSAGE("Listening for heap-leaks commands on port %d (pid 0x%x unixpid %d)\n",
        port, GetCurrentProcessId(), getpid());
    do
    {
        if ((connectfd = accept(fd, NULL, NULL)) < 0)
            break;

        while (1)
        {
            i = 0;
            nread = 0;
            memset(buffer, 0, sizeof(buffer));
            while (nread < sizeof(buffer)-1)
            {
                if ((i = recv(connectfd, &buffer[nread], 1, 0)) != 1)
                    break;
                if (buffer[nread] == '\n' ||
                    buffer[nread] == '\r')
                    break;
                nread += i;
            }
            buffer[nread] = 0;
            if (!nread) continue;

            token = strtok_r(buffer, " ", &next);

            if (!strcasecmp(token, "dump"))
            {
                /* [sort] */
                sort = LH_SORT_UNKNOWN;
                if ((token = strtok_r(NULL, " ", &next)))
                {
                    sort = lh_str_to_sort_type(token);
                    if (sort == LH_SORT_UNKNOWN)
                    {
                        lh_send(connectfd, "unknown sort: %s\n", token);
                        continue;
                    }
                }

                /* [heap] */
                heap = NULL;
                if ((token = strtok_r(NULL, " ", &next)) &&
                    !(heap = lh_parse_heap(token)))
                {
                    lh_send(connectfd, "invalid heap: %s\n", token);
                    continue;
                }

                lh_lock_all_heaps();
                if (token)
                    sort = interlocked_xchg(&lh_sort, sort);
                lh_dump(connectfd, heap);
                if (token)
                    interlocked_xchg(&lh_sort, sort);
                lh_unlock_all_heaps();
            }
            else if (!strcasecmp(token, "clear"))
            {
                lh_lock_all_heaps();
                lh_cmd_clear(connectfd);
                lh_unlock_all_heaps();
            }
            else if (!strcasecmp(token, "summary"))
            {
                lh_lock_all_heaps();
                lh_dump_summary();
                lh_unlock_all_heaps();
            }
            else if (!strcasecmp(token, "list"))
            {
                lh_lock_all_heaps();
                lh_cmd_list(connectfd);
                lh_unlock_all_heaps();
            }
            else if (!strcasecmp(token, "info"))
            {
                /* [heap] */
                heap = NULL;
                if ((token = strtok_r(NULL, " ", &next)) &&
                    !(heap = lh_parse_heap(token)))
                {
                    lh_send(connectfd, "invalid heap: %s\n", token);
                    continue;
                }

                lh_info(connectfd, heap);
            }
            else if (!strcasecmp(token, "map"))
            {
                /* [heap] */
                heap = NULL;
                if ((token = strtok_r(NULL, " ", &next)) &&
                    !(heap = lh_parse_heap(token)))
                {
                    lh_send(connectfd, "invalid heap: %s\n", token);
                    continue;
                }

                lh_map(connectfd, heap);
            }
            else if (!strcasecmp(token, "help"))
                lh_thread_help(connectfd);
            else if (!strcasecmp(token, "quit"))
                break;
            else
                lh_send(connectfd, "unknown command (try 'help'): %s\n", token);
        }

        shutdown(connectfd, 2);
        close(connectfd);
    } while (1);
    close(fd);
    return;

error:
    if (fd) close(fd);
    NtSetEvent(ready, NULL);
}

static RTL_RUN_ONCE lh_init_once = RTL_RUN_ONCE_INIT;
/* SIGQUIT will terminate thread on exit */
static DWORD WINAPI lh_start_thread(RTL_RUN_ONCE *once, void *param, void **context)
{
    HANDLE ready;
    CLIENT_ID tid;
    NTSTATUS status;
    OBJECT_ATTRIBUTES attr;

    attr.Length                   = sizeof(attr);
    attr.RootDirectory            = 0;
    attr.ObjectName               = NULL;
    attr.Attributes               = OBJ_OPENIF;
    attr.SecurityDescriptor       = NULL;
    attr.SecurityQualityOfService = NULL;

    status = NtCreateEvent(&ready, EVENT_ALL_ACCESS, &attr, NotificationEvent, FALSE);
    if (FAILED(status))
    {
        ERR_(heapleaks)("failed to create ready event: 0x%08x\n", status);
        return FALSE;
    }

    status = RtlCreateUserThread(GetCurrentProcess(), NULL, FALSE, NULL, 0, 0,
                                 lh_thread_proc, ready, &lh_thread, &tid);
    if (SUCCEEDED(status))
        status = NtWaitForMultipleObjects(1, &ready, TRUE, TRUE, NULL);
    NtClose(ready);

    if (FAILED(status) || !lh_listener_running)
    {
        ERR_(heapleaks)("failed to spawn listener thread: 0x%08x\n", status);
        exit(1);
    }

    return TRUE;
}

BOOL CDECL wine_heapleak_init(void)
{
    UNICODE_STRING oanocache;
    UNICODE_STRING libname;
    UNICODE_STRING one;
    WCHAR cmdfilt[MAX_PATH];
    WCHAR sortstr[32];
    unsigned long val;
    ANSI_STRING func;
    NTSTATUS status;
    LDR_MODULE *mod;
    void *cookie;
    void *fp;

    /* calling this init from kernel32 misses some early ntdll
       allocations but they are mostly process-long memory and
       not readlly leaks.  skipping them saves a bit in tracking */

    /* GetSystemDirectoryW is hard-coded to this */
    static const WCHAR system32[] = {'C',':','\\','w','i','n','d','o','w','s',
                                     '\\','s','y','s','t','e','m','3','2',0};
    static const WCHAR dbghelp[] = {'d','b','g','h','e','l','p','.','d','l','l',0};
    static const WCHAR oanocacheW[] = {'o','a','n','o','c','a','c','h','e',0};
    static const WCHAR oneW[] = {'o','n','e',0};
    static const WCHAR skipW[] = {'H','E','A','P','L','E','A','K','S','_','S','K','I','P',0};
    static const WCHAR minW[] = {'H','E','A','P','L','E','A','K','S','_','M','I','N',0};
    static const WCHAR maxW[] = {'H','E','A','P','L','E','A','K','S','_','M','A','X',0};
    static const WCHAR nframesW[] = {'H','E','A','P','L','E','A','K','S','_','N','F','R','A','M','E','S',0};
    static const WCHAR nprintW[] = {'H','E','A','P','L','E','A','K','S','_','N','P','R','I','N','T',0};
    static const WCHAR includeW[] = {'H','E','A','P','L','E','A','K','S','_','I','N','C','L','U','D','E',0};
    static const WCHAR dumpeachW[] = {'H','E','A','P','L','E','A','K','S','_','D','U','M','P','E','A','C','H',0};
    static const WCHAR sortbyW[] = {'H','E','A','P','L','E','A','K','S','_','S','O','R','T','B','Y',0};
    static const WCHAR cmdfiltW[] = {'H','E','A','P','L','E','A','K','S','_','C','M','D','F','I','L','T',0};
    static const WCHAR portminW[] = {'H','E','A','P','L','E','A','K','S','_','P','O','R','T','_','M','I','N',0};
    static const WCHAR portmaxW[] = {'H','E','A','P','L','E','A','K','S','_','P','O','R','T','_','M','A','X',0};


    if (!lh_getenv(cmdfiltW, cmdfilt, sizeof(cmdfilt)))
    {
        WCHAR *cmdline = NtCurrentTeb()->Peb->ProcessParameters->CommandLine.Buffer;

        if (!strstrW(cmdline, cmdfilt))
        {
            MESSAGE("not tracing leaks for pid 0x%x (unix %d) ('%s' not found in '%s')\n",
                GetCurrentProcessId(), getpid(), debugstr_w(cmdfilt), debugstr_w(cmdline));
            return TRUE;
        }
        else
        {
            MESSAGE("tracing leaks for pid 0x%x (unix %d) ('%s' found in '%s')\n",
                GetCurrentProcessId(), getpid(), debugstr_w(cmdfilt), debugstr_w(cmdline));
            /* fall-through */
        }
    }

    if (!RtlCaptureStackBackTrace(0, 1, &fp, NULL))
    {
        FIXME_(heapleaks)("RtlCaptureStackBackTrace not supported on this platform\n");
        return FALSE;
    }

    RtlInitUnicodeString(&libname, dbghelp);
    status = LdrLoadDll(system32, 0, &libname, &hdbghelp);
    if (status) goto error;

#define LOAD_FUNC(x)    \
    do {    \
        RtlInitAnsiString(&func, #x);   \
        status = LdrGetProcedureAddress(hdbghelp, &func, 0, &fp );    \
        if (status) goto error; \
        x##_func = fp;   \
    } while (0)

    LOAD_FUNC(SymSetOptions);
    LOAD_FUNC(SymInitialize);
    LOAD_FUNC(SymCleanup);
    LOAD_FUNC(SymFromAddr);
    LOAD_FUNC(SymGetLineFromAddr64);
    LOAD_FUNC(SymGetModuleInfo);
    LOAD_FUNC(SymGetModuleInfoW64);
#undef LOAD_FUNC

    /* dbghelp allocates memory from a pool that is needed when printing leaks
       so don't record them as leaks */

    /* LdrFindEntryForAddress must be called while loader lock is held
       since this initialization is done from DllMain, we already hold it */
    mod = NULL;
    if (LdrFindEntryForAddress(hdbghelp, &mod) || !mod)
        goto error;
    dbghelp_start = (DWORD_PTR)mod->BaseAddress;
    dbghelp_end = dbghelp_start + mod->SizeOfImage;
    TRACE_(heapleaks)("dbghelp: %p %lx - %lx\n", hdbghelp, dbghelp_start, dbghelp_end);

    if (!lh_getenv_ul(skipW, &val))
    {
        lh_skip = val;
        TRACE_(heapleaks)("setting skip %d\n", lh_skip);
    }
    if (!lh_getenv_ul(minW, &val))
    {
        lh_min = val;
        TRACE_(heapleaks)("setting min %ld (0x%lx)\n", lh_min, lh_min);
    }
    if (!lh_getenv_ul(maxW, &val))
    {
        lh_max = val;
        TRACE_(heapleaks)("setting max %ld (0x%lx)\n", lh_max, lh_max);
    }
    if (!lh_getenv_ul(nframesW, &val))
    {
        lh_nframes = min(val, HL_MAX_NFRAMES);
        TRACE_(heapleaks)("displaying nframes %u (0x%x)\n", lh_nframes, lh_nframes);
    }
    if (!lh_getenv_ul(nprintW, &val))
    {
        lh_nprint = val;
        TRACE_(heapleaks)("setting nprint %u (0x%x)\n", lh_nprint, lh_nprint);
    }
    if (!lh_getenv_ul(dumpeachW, &val))
    {
        lh_dump_each = val;
        if (lh_dump_each)
            TRACE_(heapleaks)("dumping each record individually\n");
        else
            TRACE_(heapleaks)("dumping groups of records\n");
    }
    if (!lh_getenv_ul(portminW, &val))
    {
        lh_port_min = val;
        TRACE_(heapleaks)("setting minimum listening port %u\n", lh_port_min);
    }
    if (!lh_getenv_ul(portmaxW, &val))
    {
        lh_port_max = val;
        TRACE_(heapleaks)("setting maximum listening port %u\n", lh_port_max);
    }

    /* don't try to load it here */
    if (!lh_getenv(includeW, lh_include_path, sizeof(lh_include_path)))
        TRACE_(heapleaks)("tracing leaks from %s\n", debugstr_w(lh_include_path));

    if (!lh_getenv(sortbyW, sortstr, sizeof(sortstr)))
    {
        static const WCHAR sizeW[] = {'s','i','z','e',0};
        static const WCHAR freqW[] = {'f','r','e','q',0};

        strlwrW(sortstr);
        if (!memcmp(sortstr, sizeW, strlenW(sortstr) * sizeof(WCHAR)))
        {
            TRACE_(heapleaks)("sorting groups of stacks by size\n");
            lh_sort = LH_SORT_SIZE;
        }
        else if (!memcmp(sortstr, freqW, strlenW(sortstr) * sizeof(WCHAR)))
        {
            TRACE_(heapleaks)("sorting groups of stacks by frequency\n");
            lh_sort = LH_SORT_FREQ;
        }
        else
        {
            ERR_(heapleaks)("invalid sort string: %s\n", debugstr_w(sortstr));
            return FALSE;
        }
    }

    /* disable the BSTR cache to avoid false positives */
    RtlInitUnicodeString(&oanocache, oanocacheW);
    RtlInitUnicodeString(&one, oneW);
    RtlSetEnvironmentVariable(NULL, &oanocache, &one);

    lh_enabled = TRUE;

    LdrEnumerateLoadedModules(NULL, lh_pin_modules, NULL);
    LdrRegisterDllNotification(0, lh_dll_pin, NULL, &cookie);

#ifdef LH_THREAD_SUPPORT
    if (!lh_port_min)
        return TRUE;

    if (!lh_port_max)
        lh_port_max = lh_port_min + 1023;
    return !RtlRunOnceExecuteOnce(&lh_init_once, lh_start_thread, NULL, NULL);
#else
    FIXME_(heapleaks)("listener thread not supported\n");
    return TRUE;
#endif

error:
    if (hdbghelp)
    {
        LdrUnloadDll(hdbghelp);
        hdbghelp = NULL;
    }
    WARN_(heapleaks)("failed to init: 0x%08x\n", status);
    return FALSE;
}

void lh_term(void)
{
    lh_dump(0, NULL); /* TODO: 0 connectfd */
}
