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
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/list.h"
#include "wine/debug.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(heap);

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
void heap_set_debug_flags_orig( HANDLE handle )
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
HANDLE WINAPI RtlCreateHeapOrig( ULONG flags, PVOID addr, SIZE_T totalSize, SIZE_T commitSize,
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

    heap_set_debug_flags_orig( subheap->heap );

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
HANDLE WINAPI RtlDestroyHeapOrig( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    SUBHEAP *subheap, *next;
    ARENA_LARGE *arena, *arena_next;
    SIZE_T size;
    void *addr;

    TRACE("%p\n", heap );
    if (!heapPtr) return heap;

    if (heap == processHeap) return heap; /* cannot delete the main process heap */

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
void * WINAPI DECLSPEC_HOTPATCH RtlAllocateHeapOrig( HANDLE heap, ULONG flags, SIZE_T size )
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
BOOLEAN WINAPI DECLSPEC_HOTPATCH RtlFreeHeapOrig( HANDLE heap, ULONG flags, void *ptr )
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
PVOID WINAPI RtlReAllocateHeapOrig( HANDLE heap, ULONG flags, PVOID ptr, SIZE_T size )
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
ULONG WINAPI RtlCompactHeapOrig( HANDLE heap, ULONG flags )
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
BOOLEAN WINAPI RtlLockHeapOrig( HANDLE heap )
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
BOOLEAN WINAPI RtlUnlockHeapOrig( HANDLE heap )
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
SIZE_T WINAPI RtlSizeHeapOrig( HANDLE heap, ULONG flags, const void *ptr )
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
BOOLEAN WINAPI RtlValidateHeapOrig( HANDLE heap, ULONG flags, LPCVOID ptr )
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
NTSTATUS WINAPI RtlWalkHeapOrig( HANDLE heap, PVOID entry_ptr )
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
ULONG WINAPI RtlGetProcessHeapsOrig( ULONG count, HANDLE *heaps )
{
    ULONG total = 1;  /* main heap */
    struct list *ptr;

    if (!processHeap)
        return 0;

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
NTSTATUS WINAPI RtlQueryHeapInformationOrig( HANDLE heap, HEAP_INFORMATION_CLASS info_class,
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

/***********************************************************************
 *           LLHEAP
 */

/*
    support for env vars:
    ESRI_LLHEAP_DISABLE                 true/false, 0/non-zero
*/

/* everything is aligned on 8 byte boundaries (16 for Win64) */
#define LLALIGNMENT              MEMORY_ALLOCATION_ALIGNMENT /* (2*sizeof(void*)) */
#define LLALIGNMENT_MASK         ((LLALIGNMENT) - 1)
#define LARGE_LLALIGNMENT        LARGE_ALIGNMENT /* large blocks have stricter alignment */
#define LLARENA_OFFSET           (LLALIGNMENT - sizeof(LLARENA_HEADER))

#define LLHEAP_MAGIC            ((DWORD)('L' | ('L'<<8) | ('H'<<16) | ('P'<<24)))
#define LLSUBHEAP_MAGIC         ((DWORD)('S' | ('U'<<8) | ('B'<<16) | ('H'<<24)))

#if LLALIGNMENT == 16
#   define LLALIGNMENT_SHIFT    4
#elif LLALIGNMENT == 8
#   define LLALIGNMENT_SHIFT    3
#else
#   error unknown alignment
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#define LLROUND_SIZE(size)      (((size) + LLALIGNMENT_MASK) & ~LLALIGNMENT_MASK)

/* TODO: try to use _SYSTEM_BASIC_INFORMATION PageSize?
         the values below are hard-coded in the virtual memory manager
         we should not use getpagesize() since we use Wine's internal vmm */
#define LLHEAP_PAGE_ALIGNMENT        0x1000
#define LLHEAP_PAGE_ALIGNMENT_MASK   (LLHEAP_PAGE_ALIGNMENT-1)

/* minimum sizes (with and without arenas) of an allocated block */
#define LLHEAP_MIN_DATA_SIZE        sizeof(LLARENA_NORMAL_DATA)
#define LLHEAP_MIN_BLOCK_SIZE       (LLROUND_SIZE(sizeof(LLARENA_NORMAL_HEADER) + LLHEAP_MIN_DATA_SIZE))

/* minimum size to start allocating large blocks: ~512KB on 32-bit, ~1MB on 64-bit */
#define LLHEAP_MIN_LARGE_BLOCK_SIZE (sizeof(void*)*128*1024 - LLHEAP_PAGE_ALIGNMENT)

/* minimum size needed to split a block into two is the
   size of a whole block plus a free arena that follows */
#define LLHEAP_MIN_SPLIT_SIZE       LLHEAP_MIN_BLOCK_SIZE

/* MUST fit in WORD */
#define LLCOMMIT_SHIFT          16
#define LLCOMMIT_SIZE           (1<<LLCOMMIT_SHIFT)
#define LLCOMMIT_MASK           (LLCOMMIT_SIZE-1)
#define LLCOMMIT_ALIGN(size)    (((size) + LLCOMMIT_MASK) & ~LLCOMMIT_MASK)

#define LLHEAP_DEF_SUBHEAP_SIZE         (1  *1024*1024)

#define LLHEAP_DEF_SUBHEAP_GROW         (1  *1024*1024)
#define LLHEAP_MAX_SUBHEAP_GROW         (16 *1024*1024) /* depends on LLARENA_SIZE_MASK being 24 bits */
#define LLHEAP_MIN_SUBHEAP_GROW         (1  *1024*1024)

#define LLHEAP_MAX_UNUSED               ((1<<8)-1) /* Max value for LLARENA_HEADER::unused */

/* decommit normal blocks this size or larger */
#define LLHEAP_DECOMMIT_THRESHOLD           (16*LLHEAP_PAGE_ALIGNMENT)

#define LLHEAP_NORMAL_FREE_LISTS            127
#define LLHEAP_UNIFORM_SIZE_LIST_IDX        64  /* block under this index are all the same size */

#define LLHEAP_LIST_BITMAP_SHIFT        5       /* DWORD is 32-bits */
#define LLHEAP_LIST_BITMAP_MASK         0x1f

#define LLHEAP_THREAD_SIZE_THRESHOLD    16384
#define LLHEAP_THREAD_LISTS             127
#define LLHEAP_THREAD_LIST_BITMAPS      ((LLHEAP_THREAD_LISTS + LLHEAP_LIST_BITMAP_MASK) >> LLHEAP_LIST_BITMAP_SHIFT)

#define LLHEAP_MIN_FAST_BLOCK_SIZE          LLROUND_SIZE(sizeof(LLARENA_FAST_BLOCK))
#define LLHEAP_NUM_GROUPS                   16   /* must be index holding blocks of same size */
#define LLHEAP_NUM_CLUSTER_BLOCKS           64
#define LLHEAP_FAST_SIZE_THRESHOLD          (LLHEAP_NUM_GROUPS << LLALIGNMENT_SHIFT)

/* buffers will be mainly what fills subheaps.  the size is chosen so that as many as possible will fit
   without wasting too much on the end of the subheap */
#define LLHEAP_BUFFER_SIZE (0xffee << LLALIGNMENT_SHIFT)

#define LLHEAP_FREE_BUFFER_WAIT     (time_t)500000 /* wait for at least this long */

#define LLHEAP_HEADER_SIZE          (LLROUND_SIZE(sizeof(LLHEAP))       + LLARENA_OFFSET)
#define LLSUBHEAP_HEADER_SIZE       (LLROUND_SIZE(sizeof(LLSUBHEAP))    + LLARENA_OFFSET)
#define LLBUFFER_HEADER_SIZE        (LLROUND_SIZE(sizeof(LLBUFFER))     + LLARENA_OFFSET)
#define LLCLUSTER_HEADER_SIZE       (LLROUND_SIZE(sizeof(LLCLUSTER))    + LLARENA_OFFSET)

#define LLARENA_FLAG_FREE           0x01 /* free applies to all blocks */
#define LLARENA_FLAG_PREV_FREE      0x02 /* previous block is free; applies only to normal blocks */

#define LLARENA_FLAG_NORMAL         0x10 /* is a normal block in a subheap */
#define LLARENA_FLAG_LARGE          0x20 /* is in the large list on the main heap */
#define LLARENA_FLAG_THREAD         0x40 /* is in per-thread buffer */
#define LLARENA_FLAG_FAST           0x80 /* is in per-thread cluster */
#define LLARENA_FLAG_TYPE_MASK      0xf0

#define LLARENA_OFFSET_MASK         0xff000000  /* depends on 16MB subheaps being 64k aligned */
#define LLARENA_OFFSET_SHIFT        24
#define LLARENA_SIZE_MASK           0x00ffffff

/* other DLLs copied these values so we have to stick to them */
#define LLARENA_INUSE_FILLER    ARENA_INUSE_FILLER
#define LLARENA_TAIL_FILLER     ARENA_TAIL_FILLER
#define LLARENA_FREE_FILLER     ARENA_FREE_FILLER

#define NOINLINE                __attribute__((noinline))

/* convention:
    req = size of request (req_size)
    blk = size of entire block (blk_size)
    act = size of actual data part (minus unused) (act_size)
    rnd = size of rounded data part (incl unused) (rnd_size)
    hdr = size of header part (hdr_size)
*/

/* generic arena header */
typedef struct tagLLARENA_HEADER
{
    DWORD   dummy;    /* Depends on block type */
    /* do NOT change these fields: */
    BYTE    flags;    /* Flags - free, type */
    BYTE    unused;   /* Unused bytes in data area (<= LLHEAP_MAX_UNUSED) */
    WORD    encoding; /* Depends on block type */
} LLARENA_HEADER;


/* this is just the header of the normal block */
typedef struct tagLLARENA_NORMAL_HEADER
{
    DWORD   off_size; /* Block offset (upper 8) and size (lower 24) */
    /* do NOT change these fields: */
    BYTE    flags;    /* Flags - free, type */
    BYTE    unused;   /* Unused bytes in data area (<= LLHEAP_MAX_UNUSED) */
    WORD    encoding; /* Encoding for block */
} LLARENA_NORMAL_HEADER;


/* this is a normal block when it's free */
typedef struct tagLLARENA_NORMAL_BLOCK
{
    DWORD   off_size;  /* Block offset (upper 8) and size (lower 24) */
    /* do NOT change these fields: */
    BYTE    flags;     /* Flags - free, type */
    BYTE    unused;    /* Unused bytes in data area (<= LLHEAP_MAX_UNUSED) */
    WORD    encoding;  /* Encoding for block */

    /* below here is in data area */
    struct list entry; /* only used when free */
} LLARENA_NORMAL_BLOCK;


/* the foot is data at the end of a block, just before the next */
typedef struct tagLLARENA_NORMAL_FOOT
{
    void *pPrevFree;   /* pointer to self when free */
} LLARENA_NORMAL_FOOT;


/* this is more documentation of what data area should
   be able to fit when a normal block is free */
typedef struct tagLLARENA_NORMAL_DATA
{
    /* start of user data if in-use */
    struct list entry; /* free list entry (if freed) */
    /* gap in user data */
    LLARENA_NORMAL_FOOT foot;   /* if free */
    /* start of next block */
} LLARENA_NORMAL_DATA;


/* header of a large block - in-use or free */
typedef struct tagLLARENA_LARGE
{
    struct list     entry;      /* Entry in heap large blocks list */
    SIZE_T          act_size;   /* Size of user data */
    SIZE_T          blk_size;   /* Total size of virtual memory block */
    /* for compatibility with normal arenas */
    DWORD           pad[2];     /* Padding to keep 16-byte alignment */
    LLARENA_HEADER  hdr;        /* Normal arena header */
} LLARENA_LARGE;

/* this is a thread block when it's free */
typedef struct tagLLARENA_THREAD_BLOCK
{
    WORD    offset;    /* Offset from start of buffer */
    WORD    blk_size;  /* Size of block */
    /* do NOT change these fields: */
    BYTE    flags;     /* Flags - free, type */
    BYTE    unused;    /* Unused bytes in data area (<= LLHEAP_MAX_UNUSED) */
    WORD    encoding;  /* Encoding for block */

    /* below here is in data area */
    struct list entry; /* only used when free */
} LLARENA_THREAD_BLOCK;

/* this is a fast block when allocated or free */
typedef struct tagLLARENA_FAST_BLOCK
{
    WORD    offset;    /* Offset from start of cluster */
    WORD    next;      /* Next free block */
    /* do NOT change these fields: */
    BYTE    flags;     /* Flags - free, type */
    BYTE    unused;    /* Unused bytes in data area (<= LLHEAP_MAX_UNUSED) */
    WORD    encoding;  /* Encoding for block */

    /* below here is in data area */
    void   *entry;     /* Note: this must (and will) be aligned on 16B on 64-bit  */
} LLARENA_FAST_BLOCK;

struct tagLLHEAP;
struct tagLLSUBHEAP;
struct tagLLPERTHREAD;
struct tagLLCLUSTER;

typedef union
{
    LLARENA_NORMAL_BLOCK  arena;
    void                 *alignment[4];
} LLFREE_LIST_ENTRY;

typedef struct tagLLBUFFER
{
    struct list            recent;      /* Returns from this thread */
    int                    nfree;       /* # of blocks freed */
    int                    nblocks;     /* # of total blocks */
    time_t                 tmfreed;     /* Time it was freed */
    void                  *other;       /* Returns from other threads */
    DWORD                  freemap[LLHEAP_THREAD_LIST_BITMAPS]; /* Bitmap to find block sooner */
    struct list            freelists[LLHEAP_THREAD_LISTS]; /* Free lists for blocks on all buffers */
    struct list            entry;       /* Entry in per-thread list */
    struct tagLLPERTHREAD *perthread;   /* Owning per-thread */
} LLBUFFER;

typedef struct tagLLGROUP
{
    DWORD                  blk_size;    /* Size of all blocks in this group */
    struct tagLLCLUSTER   *curclr;      /* Currently active cluster */
    struct list            clusters;    /* List of used clusters */
    struct list            full;        /* List of full clusters */
    struct tagLLPERTHREAD *perthread;   /* Per-thread group belongs to */
} LLGROUP;

typedef struct tagLLCLUSTER
{
    LLGROUP        *group;              /* Group that owns this cluster */
    WORD            last;               /* Last freed blocks (top of recently used stack) */
    WORD            nfree;              /* # of blocks freed */
    WORD            top;                /* Top block, before saturation */
    WORD            limit;              /* Real limit, saturated when reached */
    struct list     entry;              /* Entry in group's cluster list */
} LLCLUSTER;

typedef struct tagLLPERTHREAD
{
    DWORD               owner;          /* Owning thread */
    void               *fastother;      /* Returns from other threads */
    LLGROUP             groups[LLHEAP_NUM_GROUPS]; /* Cluster groups */
    LLBUFFER           *curbuf;         /* Current buffer */
    struct list         buffers;        /* List of buffers */
    struct list         entry;          /* Entry in heap's per-thread list */
    struct tagLLHEAP   *heap;           /* Heap this per-thread belongs */
} LLPERTHREAD;

/* subheaps are for normal blocks */
typedef struct tagLLSUBHEAP
{
    struct list             recent;
    int                     nfree;
    int                     nblocks;
    LLFREE_LIST_ENTRY       freelists[LLHEAP_NORMAL_FREE_LISTS];
    SIZE_T                  size;           /* Size of the whole sub-heap */
    SIZE_T                  min_commit;     /* Minimum committed size */
    SIZE_T                  commitSize;     /* Committed size of the sub-heap */
    struct tagLLHEAP       *heap;           /* Main heap structure */
    DWORD                   headerSize;     /* Size of the subheap header */
    DWORD                   magic;          /* Magic number */
    struct list             entry;          /* Entry in subheap list on heap */
} LLSUBHEAP;

typedef struct tagLLHEAP
{
    LLSUBHEAP                   subheap;        /* MUST be first member */
    LLSUBHEAP                  *active;         /* Active subheap */
    ULONG_PTR                   obfuscator;     /* Obfuscator for encoding blocks */
    DWORD                       tls_idx;        /* TLS index for per-threads */
    RTL_CRITICAL_SECTION        cs;             /* Critical section */
    RTL_CRITICAL_SECTION_DEBUG  cs_dbg;         /* Critical section debug info */
    char                        cs_name[32];    /* Critical section name */
    DWORD                       magic;          /* Magic number */
    DWORD                       flags;          /* Heap flags */
    DWORD                       force_flags;    /* Forced heap flags for debugging */
    SIZE_T                      grow_size;      /* Size of next subheap for growing heap */
    struct list                 subheaps;       /* List of subheaps */
    struct list                 entry;          /* Entry in process heap list */
    struct list                 large_list;     /* Large blocks list */
    struct list                 perthreads;     /* In-use per-threads list */
    struct list                 free_pts;       /* Free per-threads list */
    struct list                 free_bufs;      /* Recently freed buffers list */
    struct list                 decommit_bufs;  /* Recently decommitted buffers list */
} LLHEAP;


static LLHEAP *processLLHeap;       /* Main process heap */


/* helper for getting bools from env vars */
static inline BOOL LLHEAP_GetBoolEnv(const char *env)
{
    const char *var;

    var = getenv(env);
    return (var && (!strcasecmp(var, "true") || atoi(var)));
}


/* inline helpers - there are admittedly a bunch of these
   but they encapsulate some of the bit fidling and pointer math */
static inline SIZE_T get_offset(const void *start, const void *end)
{
    return (SIZE_T)((char *)end - (char *)start);
}

static inline void *get_addr(const void *ptr, INT_PTR offset)
{
    return (void *)((char *)ptr + offset);
}

static inline void *user_ptr_to_arena(const void *ptr)
{
    return ((LLARENA_HEADER *)ptr - 1);
}

static inline void *arena_to_user_ptr(const void *ptr)
{
    return ((LLARENA_HEADER *)ptr + 1);
}

static inline DWORD get_flags(const void *pArena)
{
    return ((const LLARENA_HEADER *)pArena)->flags;
}

static inline DWORD blk_to_rnd_size(DWORD blk_size)
{
    return blk_size - sizeof(LLARENA_HEADER);
}

static inline DWORD rnd_to_blk_size(DWORD rnd_size)
{
    return rnd_size + sizeof(LLARENA_HEADER);
}

static inline SIZE_T calc_blk_size_normal(DWORD flags, SIZE_T req_size)
{
    if (flags & HEAP_TAIL_CHECKING_ENABLED) req_size += LLALIGNMENT;
    return LLROUND_SIZE(sizeof(LLARENA_HEADER) + req_size);
}

static inline SIZE_T calc_blk_size_large(DWORD flags, SIZE_T req_size)
{
    if (flags & HEAP_TAIL_CHECKING_ENABLED) req_size += LLALIGNMENT;
    return LLROUND_SIZE(sizeof(LLARENA_LARGE) + req_size);
}

static inline DWORD get_blk_size_normal(const LLARENA_NORMAL_BLOCK *pArena)
{
    return pArena->off_size & LLARENA_SIZE_MASK;
}

static inline void set_blk_size_normal(LLARENA_NORMAL_BLOCK *pArena, DWORD size)
{
    pArena->off_size = size | (pArena->off_size & LLARENA_OFFSET_MASK);
}

static inline void set_blk_size_offset(LLARENA_NORMAL_BLOCK *pArena, DWORD size, DWORD offset)
{
    pArena->off_size = size | (offset << LLARENA_OFFSET_SHIFT);
}

static inline DWORD get_rnd_size_normal(const LLARENA_NORMAL_BLOCK *pArena)
{
    return blk_to_rnd_size(get_blk_size_normal(pArena));
}

static inline void *get_next_arena_blk(const void *pArena, SIZE_T blk_size)
{
    return get_addr(pArena, blk_size);
}

static inline void *get_next_arena(const LLARENA_NORMAL_BLOCK *pArena)
{
    return get_addr(pArena, get_blk_size_normal(pArena));
}

static inline BYTE *get_unused_ptr(const LLARENA_NORMAL_BLOCK *pArena)
{
    return get_addr(pArena, get_blk_size_normal(pArena) - pArena->unused);
}

static inline DWORD get_arena_offset(const LLARENA_NORMAL_BLOCK *pArena)
{
    return pArena->off_size >> LLARENA_OFFSET_SHIFT;
}

static inline LLSUBHEAP *get_arena_subheap(const LLARENA_NORMAL_BLOCK *pArena)
{
    return (LLSUBHEAP *)((UINT_PTR)
            ((char *)pArena - (get_arena_offset(pArena) << LLCOMMIT_SHIFT)) & ~LLCOMMIT_MASK);
}

static inline WORD get_subheap_offset(const LLSUBHEAP *subheap, const LLARENA_NORMAL_BLOCK *pArena)
{
    return (WORD)(((char *)pArena - (char *)subheap) >> LLCOMMIT_SHIFT);
}

static inline const void *get_subheap_limit(const LLSUBHEAP *subheap)
{
    return get_addr(subheap, subheap->size);
}

static inline void *get_first_block(const LLSUBHEAP *subheap)
{
    return get_addr(subheap, subheap->headerSize);
}

/* NOTE: gets the foot of the previous block */
static inline LLARENA_NORMAL_FOOT *get_foot(const void *pArena)
{
    return ((LLARENA_NORMAL_FOOT *)pArena) - 1;
}

static inline BOOL is_last_in_subheap(const LLSUBHEAP *subheap, LLARENA_NORMAL_BLOCK *pArena)
{
    return get_addr(get_next_arena(pArena), sizeof(LLARENA_NORMAL_BLOCK)) >= get_subheap_limit(subheap) ? TRUE : FALSE;
}

static inline ULONG get_normal_list_index(SIZE_T blk_size)
{
#ifdef __x86_64__
        if ((blk_size >> 4)  < LLHEAP_UNIFORM_SIZE_LIST_IDX) return  blk_size >> 4;
        if ((blk_size >> 5)  < 32) return (blk_size >> 5) + 32;
        if ((blk_size >> 6)  < 32) return (blk_size >> 6) + 64;
        if ((blk_size >> 7)  < 24) return (blk_size >> 7) + 88;
        if ((blk_size >> 8)  < 16) return (blk_size >> 8) + 104;
        if ((blk_size >> 10) <  5) return (blk_size >> 10) + 119;
#else
        if ((blk_size >> 3) < LLHEAP_UNIFORM_SIZE_LIST_IDX) return  blk_size >> 3;
        if ((blk_size >> 4) < 32) return (blk_size >> 4) + 32;
        if ((blk_size >> 5) < 32) return (blk_size >> 5) + 64;
        if ((blk_size >> 6) < 24) return (blk_size >> 6) + 88;
        if ((blk_size >> 7) < 16) return (blk_size >> 7) + 104;
        if ((blk_size >> 9) <  5) return (blk_size >> 9) + 119;
#endif
    return LLHEAP_NORMAL_FREE_LISTS-1;
}

static inline ULONG get_arena_list_index(const LLARENA_NORMAL_BLOCK *pArena)
{
    return get_normal_list_index(get_blk_size_normal(pArena));
}

static void NOINLINE CDECL zero_memory(void *ptr, size_t size)
{
    memset(ptr, 0, size);
}

static void NOINLINE CDECL fill_memory(void *dst, int fill, size_t size)
{
    memset(dst, fill, size);
}

static void NOINLINE CDECL copy_memory(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
}

static time_t NOINLINE CDECL current_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * (time_t)1000000 + tv.tv_usec;
}

/* initialize contents of a newly created block of memory */
static inline void LLHEAP_initialize_block( void *ptr, SIZE_T size, DWORD flags )
{
    if (flags & HEAP_ZERO_MEMORY)
        zero_memory(ptr, size);
}

static inline void LLHEAP_UnsetPrevFree(LLARENA_NORMAL_BLOCK *pFree, const void *limit)
{
    LLARENA_NORMAL_BLOCK *pNext;

    pNext = get_next_arena(pFree);
    if ((const void *)pNext < limit)
        pNext->flags &= ~LLARENA_FLAG_PREV_FREE;
}

/* returns the free block, if any, before the given one */
static inline void *LLHEAP_GetPrevFreeBlock(const void *pArena)
{
    if ((get_flags(pArena) & LLARENA_FLAG_PREV_FREE))
        return get_foot(pArena)->pPrevFree;
    return NULL;
}

static inline DWORD LLHEAP_BlockTypeFromSize(SIZE_T blk_size)
{
    if (blk_size < LLHEAP_FAST_SIZE_THRESHOLD) return LLARENA_FLAG_FAST;
    if (blk_size < LLHEAP_THREAD_SIZE_THRESHOLD) return LLARENA_FLAG_THREAD;
    if (blk_size < LLHEAP_MIN_LARGE_BLOCK_SIZE) return LLARENA_FLAG_NORMAL;
    return LLARENA_FLAG_LARGE;
}

static inline DWORD LLHEAP_BlockType(const void *pArena)
{
    return get_flags(pArena) & LLARENA_FLAG_TYPE_MASK;
}

static inline DWORD get_blk_size_thread(const LLARENA_THREAD_BLOCK *block)
{
    return block->blk_size << LLALIGNMENT_SHIFT;
}

static inline void set_blk_size_thread(LLARENA_THREAD_BLOCK *block, SIZE_T blk_size)
{
    block->blk_size = blk_size >> LLALIGNMENT_SHIFT;
}

static inline void *get_thread_unused_ptr(const LLARENA_THREAD_BLOCK *block, DWORD req_size)
{
    return get_addr(block, sizeof(LLARENA_HEADER) + req_size);
}

static inline const void *get_buffer_limit(const LLBUFFER *buffer)
{
    return get_addr(buffer, LLHEAP_BUFFER_SIZE);
}

static inline int is_past_buffer(const LLBUFFER *buffer, const void *block)
{
    /* because of shifting, the block size for the last block is truncated
       so the next block may not touch the limit */
    return get_addr(block, LLHEAP_MIN_BLOCK_SIZE) >= get_buffer_limit(buffer);
}

static inline LLARENA_THREAD_BLOCK *get_first_buffer_block(const LLBUFFER *buffer)
{
    return get_addr(buffer, LLBUFFER_HEADER_SIZE);
}

static inline LLBUFFER *get_buffer(const LLARENA_THREAD_BLOCK *blk)
{
    return get_addr(blk, -(INT_PTR)((blk->offset << LLALIGNMENT_SHIFT) + LLBUFFER_HEADER_SIZE));
}

static inline WORD get_buffer_offset(const LLBUFFER *buffer, const LLARENA_THREAD_BLOCK *blk)
{
    return (WORD)(((char *)blk - (char*)buffer - LLBUFFER_HEADER_SIZE) >> LLALIGNMENT_SHIFT);
}

static inline ULONG get_thread_list_index(SIZE_T blk_size)
{
        if ((blk_size >> LLALIGNMENT_SHIFT) < 64) return  blk_size >> LLALIGNMENT_SHIFT;
#ifdef __x86_64__
        if ((blk_size >> 5)  < 32) return (blk_size >> 5) + 32;
        if ((blk_size >> 6)  < 32) return (blk_size >> 6) + 64;
        if ((blk_size >> 7)  < 24) return (blk_size >> 7) + 88;
        if ((blk_size >> 8)  < 16) return (blk_size >> 8) + 104;
        if ((blk_size >> 10) <  5) return (blk_size >> 10) + 119;
#else
        if ((blk_size >> 4) < 32) return (blk_size >> 4) + 32;
        if ((blk_size >> 5) < 32) return (blk_size >> 5) + 64;
        if ((blk_size >> 6) < 24) return (blk_size >> 6) + 88;
        if ((blk_size >> 7) < 16) return (blk_size >> 7) + 104;
        if ((blk_size >> 9) <  5) return (blk_size >> 9) + 119;
#endif
    return LLHEAP_THREAD_LISTS-1;
}

static inline void set_free_idx(DWORD *map, int idx)
{
    map[idx >> LLHEAP_LIST_BITMAP_SHIFT] |= (1 << (idx & LLHEAP_LIST_BITMAP_MASK));
}

static inline int have_other_thread_blocks(void **other)
{
    return !!(*(volatile void **)other);
}

static inline void small_pause(void)
{
#ifdef __i386__
    __asm__ __volatile__( "rep;nop" : : : "memory" );
#else
    __asm__ __volatile__( "pause" : : : "memory" );
#endif
}

static inline void push_thread_block(void **other, void **entry)
{
    void *old;
    for (;;)
    {
        old = *other;
        *entry = old;
        if (interlocked_cmpxchg_ptr(other, entry, old) == old)
            break;
        small_pause();
    }
}

static inline void *flush_thread_blocks(void **other)
{
    return interlocked_xchg_ptr(other, NULL);
}

/* Note: this must change if get_thread_list_index does */
static inline LLGROUP *get_group(LLPERTHREAD *perthread, SIZE_T blk_size)
{
    return &perthread->groups[blk_size >> LLALIGNMENT_SHIFT];
}

/* Note: this must change if get_thread_list_index does */
static inline DWORD get_group_blk_size(int idx)
{
    return idx << LLALIGNMENT_SHIFT;
}

static inline LLARENA_FAST_BLOCK *get_cluster_block(const LLCLUSTER *cluster, int idx)
{
    return get_addr(cluster, idx);
}

static inline LLCLUSTER *get_cluster(const LLARENA_FAST_BLOCK *block)
{
    return get_addr(block, -(INT_PTR)block->offset);
}

/***********************************************************************
 *           LLHEAP_TlsAlloc
 *
 * Subset of TlsAlloc() in kernel32/process.c
 * We can't include/link against kernel32 (circular dependency with ntdll)
 * so we copy what we need with some modifications
 */
static DWORD CDECL LLHEAP_TlsAlloc(void)
{
    DWORD index;
    PEB * const peb = NtCurrentTeb()->Peb;

    RtlAcquirePebLock();
    index = RtlFindClearBitsAndSet( peb->TlsBitmap, 1, 0 );
    if (index != TLS_OUT_OF_INDEXES) NtCurrentTeb()->TlsSlots[index] = 0; /* clear the value */
    /* TODO: maybe support expansion slots */
    RtlReleasePebLock();
    return index;
}

static BOOL CDECL LLHEAP_TlsFree(DWORD index)
{
    BOOL ret;

    ret = 0;
    RtlAcquirePebLock();
    /* TODO: maybe support expansion slots */
    if (index < TLS_MINIMUM_AVAILABLE)
    {
        ret = RtlAreBitsSet( NtCurrentTeb()->Peb->TlsBitmap, index, 1 );
        if (ret) RtlClearBits( NtCurrentTeb()->Peb->TlsBitmap, index, 1 );
    }
    if (ret) NtSetInformationThread( GetCurrentThread(), ThreadZeroTlsCell, &index, sizeof(index) );
    else SetLastError( ERROR_INVALID_PARAMETER );
    RtlReleasePebLock();
    return ret;
}

static inline LPVOID CDECL LLHEAP_TlsGetValue(DWORD index)
{
    return NtCurrentTeb()->TlsSlots[index];
}

static inline void CDECL LLHEAP_TlsSetValue(DWORD index, LPVOID value)
{
    NtCurrentTeb()->TlsSlots[index] = value;
}

/***********************************************************************
 *           LLHEAP_GetPerThread
 */
static inline LLPERTHREAD *CDECL LLHEAP_GetPerThread(LLHEAP *heap)
{
    return LLHEAP_TlsGetValue(heap->tls_idx);
}

/***********************************************************************
 *          LLHEAP_GetObfuscator
 */
static ULONG_PTR CDECL LLHEAP_GetObfuscator(const void *heap)
{
    ULONG seed;
    ULONG_PTR rand;

    /* see get_pointer_obfuscator in rtl.c */

    seed = NtGetTickCount();
    rand = RtlUniform(&seed);
    rand ^= (ULONG_PTR)RtlUniform(&seed) << ((sizeof (DWORD_PTR) - sizeof (ULONG))*8);
    rand ^= (ULONG_PTR)heap;
    return rand;
}

/***********************************************************************
 *          LLHEAP_Encode
 */
static inline WORD CDECL LLHEAP_Encode(const LLHEAP *heap, const void *pArena)
{
    const LLARENA_HEADER *block = pArena;

    /* we don't include PREV_FREE bit since allocating and freeing previous block
       will change this bit, making the encoding invalid.  alternatively, actions
       on the previous block could update the encoding on the next block */
    if (heap->flags & HEAP_VALIDATE)
        return (WORD)(heap->obfuscator ^ block->dummy ^ block->unused ^ (ULONG_PTR)pArena ^
                     (block->flags & ~LLARENA_FLAG_PREV_FREE));
    else
        return 0; /* still need to have a known value in case HeapValidate is called */
}

/***********************************************************************
 *          LLHEAP_MarkTail
 */
static inline void CDECL LLHEAP_MarkTail(const void *block, SIZE_T blk_size, SIZE_T unused)
{
    fill_memory(get_addr(block, blk_size - unused), LLARENA_TAIL_FILLER, unused);
}

/***********************************************************************
 *           LLHEAP_IsEnabled
 */
static inline BOOL LLHEAP_IsEnabled(void)
{
    static BOOL enabled = -1;

    if (enabled == -1)
        /* llheap can only be enabled if not disabled and futexes are supported */
        enabled = !LLHEAP_GetBoolEnv("ESRI_LLHEAP_DISABLE");
    return enabled;
}

/***********************************************************************
 *           LLHEAP_IsOrigHeap
 */
static inline BOOL LLHEAP_IsOrigHeap(const HANDLE handle)
{
    const HEAP *heap = (const HEAP *)handle;
    return heap->magic == HEAP_MAGIC;
}

/***********************************************************************
 *           LLHEAP_UseOrigHeap
 */
static inline BOOL LLHEAP_UseOrigHeap(const void *addr, DWORD flags, SIZE_T size)
{
                                                                 /* use original heap if... */
    return  (!LLHEAP_IsEnabled() ||                              /* llheap is disabled completely */
             (flags & HEAP_NO_SERIALIZE) ||                      /* non-serialized - back-end must be serialized */
             ((DWORD_PTR)addr & LLCOMMIT_MASK) ||                /* not 64k aligned - subheap offsets need this */
             (!(flags & HEAP_GROWABLE) && size) ||               /* not growable */
             (LLCOMMIT_ALIGN(size) > LLHEAP_MAX_SUBHEAP_GROW));  /* heap too big - block offsets need this */
}

/***********************************************************************
 *           LLHEAP_ValidateBlockCommon
 */
static BOOL CDECL LLHEAP_ValidateBlockCommon(LLHEAP *heap, const LLARENA_HEADER *header, DWORD type)
{
    DWORD blocktype;
    WORD encoding;
    void *ptr;

    /* check alignment */
    ptr = arena_to_user_ptr(header);
    if (((UINT_PTR)ptr & LLALIGNMENT_MASK))
    {
        ERR("%u:%lu: Heap %p: Bad alignment on %p\n",
            getpid(), pthread_self(), heap, header);
        return FALSE;
    }

    /* check encoding */
    encoding = LLHEAP_Encode(heap, header);
    if (header->encoding != encoding)
    {
        ERR("%u:%lu: Heap %p: Bad encoding %p: found %x calculated %x\n",
            getpid(), pthread_self(), heap, header, header->encoding, encoding);
        return FALSE;
    }

    /* check type */
    blocktype = LLHEAP_BlockType(header);
    if (blocktype != type)
    {
        ERR("%u:%lu: Heap %p: Bad type on %p: found %x should be %x\n",
            getpid(), pthread_self(), heap, header, blocktype, type);
        return FALSE;
    }

    return TRUE;
}


/***********************************************************************
 *           LLHEAP_ValidateSubheap
 */
static BOOL CDECL LLHEAP_ValidateSubheap(LLHEAP *heap, const LLSUBHEAP *subheap)
{
    SIZE_T headerSize;

    if ((INT_PTR)subheap & LLCOMMIT_MASK)
    {
        ERR("%u:%lu: Heap %p: Subheap %p does not have commit alignment %08x\n",
            getpid(), pthread_self(), heap, subheap, LLCOMMIT_SIZE);
        return FALSE;
    }

    if (subheap->magic != LLSUBHEAP_MAGIC)
    {
        ERR("%u:%lu: Heap %p: Subheap %p has invalid magic: should be 0x%08x is 0x%08x\n",
            getpid(), pthread_self(), heap, subheap, LLSUBHEAP_MAGIC, subheap->magic);
        return FALSE;
    }

    if (subheap->heap != heap)
    {
        ERR("%u:%lu: Heap %p: Subheap: %p: Invalid heap pointer %p\n",
            getpid(), pthread_self(), heap, subheap, subheap->heap);
        return FALSE;
    }

    /* There is no max size */
    if (subheap->min_commit < subheap->headerSize)
    {
        ERR("%u:%lu: Heap %p: Subheap: %p: Min commit too small: min_commit %08lx header %08x\n",
            getpid(), pthread_self(), heap, subheap, subheap->min_commit, subheap->headerSize);
        return FALSE;
    }

    if (subheap->commitSize < subheap->headerSize)
    {
        ERR("%u:%lu: Heap %p: Subheap: %p: Commited size under header: commitSize %08lx header %08lx\n",
            getpid(), pthread_self(), heap, subheap, subheap->commitSize, subheap->size);
        return FALSE;
    }

    if (subheap->commitSize > subheap->size)
    {
        ERR("%u:%lu: Heap %p: Subheap: %p: Committed size more than size: commitSize %08lx size %08lx\n",
            getpid(), pthread_self(), heap, subheap, subheap->commitSize, subheap->size);
        return FALSE;
    }

    headerSize = ((void*)subheap == (void*)subheap->heap) ? LLHEAP_HEADER_SIZE : LLSUBHEAP_HEADER_SIZE;
    if (subheap->headerSize != headerSize)
    {
        ERR("%u:%lu: Heap %p: Subheap: %p: Invalid header size: should be %08lx is %08x\n",
            getpid(), pthread_self(), heap, subheap, headerSize, subheap->headerSize);
        return FALSE;
    }

    return TRUE;
}


/***********************************************************************
 *           LLHEAP_ValidateNormalBlock
 */
static BOOL CDECL LLHEAP_ValidateNormalBlock(LLHEAP *heap, const LLARENA_NORMAL_BLOCK *pArena, BOOL isfree)
{
    DWORD arena_flags;
    SIZE_T rnd_size;
    BYTE *unused;
    DWORD i;

    if (!LLHEAP_ValidateBlockCommon(heap, (const LLARENA_HEADER *)pArena, LLARENA_FLAG_NORMAL))
        return FALSE;

    rnd_size = get_rnd_size_normal(pArena);
    if (pArena->unused > rnd_size)
    {
        ERR("%u:%lu: Heap %p: Unused bytes too large on %p: unused %u rnd_size %lu\n",
            getpid(), pthread_self(), heap, pArena, pArena->unused, rnd_size);
        return FALSE;
    }

    arena_flags = get_flags(pArena);
    if (!isfree)
    {
        if (arena_flags & LLARENA_FLAG_FREE)
        {
            ERR("%u:%lu: Heap %p: In-use block %p marked as free\n",
                getpid(), pthread_self(), heap, pArena);
            return FALSE;
        }

        if (heap->flags & HEAP_TAIL_CHECKING_ENABLED)
        {
            unused = get_unused_ptr(pArena);
            for (i = 0; i < pArena->unused; i++)
            {
                if (unused[i] != LLARENA_TAIL_FILLER)
                {
                    ERR("%u:%lu: Heap %p: Bad tail on %p on byte %i: %x\n",
                        getpid(), pthread_self(), heap, pArena, i, unused[i]);
                    return FALSE;
                }
            }
        }
    }
    else if (isfree && !(arena_flags & LLARENA_FLAG_FREE))
    {
        ERR("%u:%lu: Heap %p: Free block %p marked as in-use\n",
            getpid(), pthread_self(), heap, pArena);
        return FALSE;
    }

    return TRUE;
}


/***********************************************************************
 *           LLHEAP_ValidateSubheapBlocks
 */
static BOOL CDECL LLHEAP_ValidateSubheapBlocks(LLHEAP *heap, const LLSUBHEAP *subheap)
{
    const LLARENA_NORMAL_BLOCK *limit, *block;

    if (!LLHEAP_ValidateSubheap(heap, subheap))
        return FALSE;

    /* subheap is safe, now check every block */
    block = get_first_block(subheap);
    limit = get_subheap_limit(subheap);
    while (block < limit)
    {
        if (!LLHEAP_ValidateNormalBlock(heap, block, get_flags(block) & LLARENA_FLAG_FREE))
            return FALSE;
        block = get_next_arena(block);
    }

    return TRUE;
}


/***********************************************************************
 *           LLHEAP_ValidateLargeBlock
 */
static BOOL CDECL LLHEAP_ValidateLargeBlock(LLHEAP *heap, LPCVOID ptr)
{
    LLARENA_HEADER *pArena;
    LLARENA_LARGE *large;
    BYTE *bytes;
    SIZE_T unused;
    SIZE_T i;

    pArena = user_ptr_to_arena(ptr);
    if (!LLHEAP_ValidateBlockCommon(heap, pArena, LLARENA_FLAG_LARGE))
        return FALSE;

    if (heap->flags & HEAP_TAIL_CHECKING_ENABLED)
    {
        large  = (LLARENA_LARGE *)ptr - 1;
        unused = large->blk_size - large->act_size - sizeof(*large);
        bytes  = get_addr(ptr, large->act_size);
        for (i = 0; i < unused; i++)
        {
            if (bytes[i] != LLARENA_TAIL_FILLER)
            {
                ERR("%u:%lu: Heap %p: Bad tail on %p on byte %li: %x\n",
                    getpid(), pthread_self(), heap, pArena, (long)i, bytes[i]);
                return FALSE;
            }
        }
    }

    return TRUE;
}


/***********************************************************************
 *           LLHEAP_ValidateHeapBlocks
 */
static BOOL CDECL LLHEAP_ValidateHeapBlocks(LLHEAP *heap)
{
    LLSUBHEAP *subheap;

    /* check subheaps */
    LIST_FOR_EACH_ENTRY(subheap, &heap->subheaps, LLSUBHEAP, entry)
    {
        if (!LLHEAP_ValidateSubheapBlocks(heap, subheap))
            return FALSE;
    }

    return TRUE;
}

/***********************************************************************
 *           LLHEAP_ValidateThreadBlock
 */
static BOOL CDECL LLHEAP_ValidateThreadBlock(LLHEAP *heap, const LLARENA_THREAD_BLOCK *block, BOOL isfree)
{
    LLARENA_NORMAL_FOOT *foot;
    DWORD arena_flags;
    LLBUFFER *buffer;
    const DWORD *end;
    DWORD blk_size;
    DWORD rnd_size;
    DWORD req_size;
    BYTE *unused;
    DWORD i;

    if (!LLHEAP_ValidateBlockCommon(heap, (const LLARENA_HEADER *)block, LLARENA_FLAG_THREAD))
        return FALSE;

    blk_size = get_blk_size_thread(block);
    rnd_size = blk_to_rnd_size(blk_size);
    if (block->unused > rnd_size)
    {
        ERR("%u:%lu: Heap %p: Unused bytes too large on thread block %p: unused %u rnd_size %u\n",
            getpid(), pthread_self(), heap, block, block->unused, rnd_size);
        return FALSE;
    }

    arena_flags = get_flags(block);
    if (!isfree)
    {
        if (arena_flags & LLARENA_FLAG_FREE)
        {
            ERR("%u:%lu: Heap %p: In-use thread block %p marked as free\n",
                getpid(), pthread_self(), heap, block);
            return FALSE;
        }

        if (heap->flags & HEAP_TAIL_CHECKING_ENABLED)
        {
            req_size = rnd_size - block->unused;
            unused = get_thread_unused_ptr(block, req_size);
            for (i = 0; i < block->unused; i++)
            {
                if (unused[i] != LLARENA_TAIL_FILLER)
                {
                    ERR("%u:%lu: Heap %p: Bad tail on thread block %p on byte %i: %x\n",
                        getpid(), pthread_self(), heap, block, i, unused[i]);
                    return FALSE;
                }
            }
        }
    }
    else /* should be free */
    {
        if (!(arena_flags & LLARENA_FLAG_FREE))
        {
            ERR("%u:%lu: Heap %p: Free thread block %p marked as in-use\n",
                getpid(), pthread_self(), heap, block);
            return FALSE;
        }

        /* check footer */
        end = get_next_arena_blk(block, blk_size);
        buffer = get_buffer(block);
        if (!is_past_buffer(buffer, end))
        {
            foot = get_foot(end);
            if (block != foot->pPrevFree)
            {
                ERR("%u:%lu: Heap %p: Bad prev on thread block %p: %p end %p foot %p\n",
                    getpid(), pthread_self(), heap, block, foot->pPrevFree, end, foot);
                return FALSE;
            }
        }
    }

    return TRUE;
}

/***********************************************************************
 *           LLHEAP_ValidateFastBlock
 */
static BOOL CDECL LLHEAP_ValidateFastBlock(LLHEAP *heap, const LLARENA_FAST_BLOCK *block, BOOL isfree)
{
    LLGROUP *group;
    LLCLUSTER *cluster;
    DWORD arena_flags;
    DWORD rnd_size;
    BYTE *unused;
    DWORD i;

    if (!LLHEAP_ValidateBlockCommon(heap, (const LLARENA_HEADER *)block, LLARENA_FLAG_FAST))
        return FALSE;

    cluster = get_cluster(block);
    group   = cluster->group;

    rnd_size = blk_to_rnd_size(group->blk_size);
    if (block->unused > rnd_size)
    {
        ERR("%u:%lu: Heap %p: Unused bytes too large on thread block %p: unused %u rnd_size %u\n",
            getpid(), pthread_self(), heap, block, block->unused, rnd_size);
        return FALSE;
    }

    arena_flags = get_flags(block);
    if (!isfree)
    {
        if (arena_flags & LLARENA_FLAG_FREE)
        {
            ERR("%u:%lu: Heap %p: In-use fast block %p marked as free\n",
                getpid(), pthread_self(), heap, block);
            return FALSE;
        }

        if (heap->flags & HEAP_TAIL_CHECKING_ENABLED)
        {
            unused = get_addr(block, group->blk_size - block->unused);
            for (i = 0; i < block->unused; i++)
            {
                if (unused[i] != LLARENA_TAIL_FILLER)
                {
                    ERR("%u:%lu: Heap %p: Bad tail on fast block %p on byte %i: %x\n",
                        getpid(), pthread_self(), heap, block, i, unused[i]);
                    return FALSE;
                }
            }
        }
    }
    else /* should be free */
    {
        if (!(arena_flags & LLARENA_FLAG_FREE))
        {
            ERR("%u:%lu: Heap %p: Free fast block %p marked as in-use\n",
                getpid(), pthread_self(), heap, block);
            return FALSE;
        }
    }

    return TRUE;
}

/***********************************************************************
 *           LLHEAP_IsRealArena  [Internal]
 * Validates a block is a valid arena.
 *
 * Locks needed in:
 * Locks held temp:
 * Locks on return:
 *
 * RETURNS
 *	TRUE: Success
 *	FALSE: Failure
 */
static BOOL CDECL LLHEAP_IsRealArena( LLHEAP *heap,   /* [in] ptr to the heap */
                      DWORD flags,   /* [in] Bit flags that control access during operation */
                      LPCVOID block, /* [in] Optional pointer to memory block to validate */
                      BOOL quiet )   /* [in] Flag - if true, HEAP_ValidateInUseArena
                                      *             does not complain    */
{
    LLARENA_LARGE *large_arena;
    BOOL ret;

    flags |= heap->flags;

    /* calling HeapLock may result in infinite recursion, so do the critsect directly */
    RtlEnterCriticalSection(&heap->cs);

    /* check memory block, if given */
    ret = TRUE;
    if (block)  /* only check this single memory block */
    {
        const void *pArena = user_ptr_to_arena(block);
        switch (LLHEAP_BlockType(pArena))
        {
            case LLARENA_FLAG_FAST:
                ret = LLHEAP_ValidateFastBlock(heap, pArena, get_flags(&block) & LLARENA_FLAG_FREE);
                break;
            case LLARENA_FLAG_THREAD:
                ret = LLHEAP_ValidateThreadBlock(heap, pArena, get_flags(pArena) & LLARENA_FLAG_FREE);
                break;
            case LLARENA_FLAG_NORMAL:
                ret = LLHEAP_ValidateNormalBlock(heap, pArena, get_flags(pArena) & LLARENA_FLAG_FREE);
                break;
            case LLARENA_FLAG_LARGE:
                ret = LLHEAP_ValidateLargeBlock(heap, block);
                break;
            default:
                ERR("%u:%lu: Heap %p: Invalid block type on %p\n",
                    getpid(), pthread_self(), heap, block);
                ret = FALSE;
                break;
        }

        RtlLeaveCriticalSection(&heap->cs);
        return ret;
    }

    if (!(ret = (heap->grow_size >= LLHEAP_MIN_SUBHEAP_GROW) && (heap->grow_size <= LLHEAP_MAX_SUBHEAP_GROW)))
        ERR("%u:%lu: Heap %p: Invalid grow size %08lx\n", getpid(), pthread_self(), heap, heap->grow_size);

    /* make sure we're the first subheap */
    if (!(ret = ((LLHEAP *)&heap->subheap == heap)))
        ERR("%u:%lu: Heap %p: not first subheap: %p\n", getpid(), pthread_self(), heap, &heap->subheap);

    /* validate all blocks on subheaps */
    if (ret)
        ret = LLHEAP_ValidateHeapBlocks(heap);

    /* check large blocks */
    if (ret)
    {
        LIST_FOR_EACH_ENTRY( large_arena, &heap->large_list, LLARENA_LARGE, entry )
            if (!(ret = LLHEAP_ValidateLargeBlock(heap, large_arena+1))) break;
    }

    RtlLeaveCriticalSection(&heap->cs);

    return ret;
}


/***********************************************************************
 *           LLHEAP_GetPtr
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
 *
 * RETURNS
 *	Pointer to the heap
 *	NULL: Failure
 */
static inline LLHEAP *LLHEAP_GetPtr(
             HANDLE heap /* [in] Handle to the heap */
)
{
    LLHEAP *heapPtr = heap;
    if ((heapPtr->flags & HEAP_VALIDATE_ALL) &&
        !LLHEAP_IsRealArena( heapPtr, 0, NULL, NOISY ))
    {
        return NULL;
    }
    return heapPtr;
}

static void fixup_clusters(struct list *clusters, const LLGROUP *group)
{
    LLARENA_FAST_BLOCK *fast;
    LLCLUSTER *cluster;
    LLHEAP *heap;
    int i;

    heap = group->perthread->heap;
    LIST_FOR_EACH_ENTRY(cluster, clusters, LLCLUSTER, entry)
    {
        for (i = LLCLUSTER_HEADER_SIZE; i < cluster->top; i += group->blk_size)
        {
            fast = get_cluster_block(cluster, i);
            fast->encoding = LLHEAP_Encode(heap, fast);
            if ((heap->flags & HEAP_TAIL_CHECKING_ENABLED) && !(fast->flags & LLARENA_FLAG_FREE))
                LLHEAP_MarkTail(fast, group->blk_size, fast->unused);
        }
    }
}

/***********************************************************************
 *           heap_set_debug_flags
 */
void heap_set_debug_flags( HANDLE handle )
{
    LLHEAP *heap;
    ULONG global_flags;
    ULONG flags;

    if (LLHEAP_IsOrigHeap(handle))
    {
        heap_set_debug_flags_orig(handle);
        return;
    }

    heap = LLHEAP_GetPtr( handle );
    global_flags = RtlGetNtGlobalFlags();
    flags = 0;

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

    heap->flags |= flags;
    heap->force_flags |= flags & ~(HEAP_VALIDATE | HEAP_DISABLE_COALESCE_ON_FREE);

    /* fix existing blocks */
    if (flags & (HEAP_VALIDATE | HEAP_TAIL_CHECKING_ENABLED))
    {
        const LLARENA_NORMAL_BLOCK *limit;
        LLARENA_THREAD_BLOCK *thread;
        LLARENA_NORMAL_BLOCK *block;
        LLPERTHREAD *perthread;
        LLSUBHEAP *subheap;
        LLBUFFER *buffer;
        LLGROUP *group;
        DWORD blk_size;
        int i;

        LIST_FOR_EACH_ENTRY(subheap, &heap->subheaps, LLSUBHEAP, entry)
        {
            block = get_first_block(subheap);
            limit = get_subheap_limit(subheap);
            while (block < limit)
            {
                block->encoding = LLHEAP_Encode(heap, block);
                if ((flags & HEAP_TAIL_CHECKING_ENABLED) && !(block->flags & LLARENA_FLAG_FREE))
                    LLHEAP_MarkTail(block, get_blk_size_normal(block), block->unused);
                block = get_next_arena(block);
            }
        }

        /* this is called while creating a new heap, so we don't need
           to worry about other thread's blocks */
        perthread = LLHEAP_GetPerThread(heap);
        if (perthread)
        {
            LIST_FOR_EACH_ENTRY(buffer, &perthread->buffers, LLBUFFER, entry)
            {
                thread = get_first_buffer_block(buffer);
                while (!is_past_buffer(buffer, thread))
                {
                    blk_size = get_blk_size_thread(thread);
                    thread->encoding = LLHEAP_Encode(heap, thread);

                    if ((flags & HEAP_TAIL_CHECKING_ENABLED) && !(thread->flags & LLARENA_FLAG_FREE))
                        LLHEAP_MarkTail(thread, blk_size, thread->unused);

                    thread = get_next_arena_blk(thread, blk_size);
                }
            }

            for (i = 0; i < LLHEAP_NUM_GROUPS; i++)
            {
                group = &perthread->groups[i];
                fixup_clusters(&group->clusters, group);
                fixup_clusters(&group->full, group);
            }
        }
    }
}


/***********************************************************************
 *           LLHEAP_InsertBlockIntoFreeLists
 */
static void CDECL LLHEAP_InsertBlockIntoFreeLists(LLSUBHEAP *subheap, LLARENA_NORMAL_BLOCK *pFree)
{
    const LLARENA_NORMAL_BLOCK *limit;
    LLARENA_NORMAL_BLOCK *pNext;
    LLARENA_NORMAL_FOOT *pFoot;
    LLFREE_LIST_ENTRY *pEntry;
    struct list *cur;
    SIZE_T blk_size;
    int idx;

    pFree->unused = 0;
    pFree->flags |= LLARENA_FLAG_FREE;
    pFree->encoding = LLHEAP_Encode(subheap->heap, pFree);

    pNext = get_next_arena(pFree);
    limit = get_subheap_limit(subheap);

    blk_size = get_blk_size_normal(pFree);
    idx      = get_normal_list_index(blk_size);
    pEntry   = &subheap->freelists[idx];
    if (pNext >= limit)
    {
        /* always blocks at end of subheap last in the list
           so it is least likely to be used and we can decommit
           the end of the subheap */
        ++pEntry;
        if (pEntry == &subheap->freelists[LLHEAP_NORMAL_FREE_LISTS])
            pEntry = subheap->freelists;
        list_add_before( &pEntry->arena.entry, &pFree->entry );
    }
    else
    {
        pNext->flags |= LLARENA_FLAG_PREV_FREE;
        pFoot = get_foot(pNext);
        pFoot->pPrevFree = pFree;

        if (idx < LLHEAP_UNIFORM_SIZE_LIST_IDX)
        {
            /* put same-sized blocks at head of list */
            list_add_after( &pEntry->arena.entry, &pFree->entry);
        }
        else
        {
            /* sort larger blocks by size to reduce fragmentation
               if blocks are the same size, the recently used comes first */
            cur = &pEntry->arena.entry;
            while ((cur = list_next(&subheap->freelists[0].arena.entry, cur)))
            {
                pNext = LIST_ENTRY(cur, LLARENA_NORMAL_BLOCK, entry);
                if (blk_size <= get_blk_size_normal(pNext))
                {
                    list_add_before( &pNext->entry, &pFree->entry );
                    return;
                }
            }

            if (!cur)
            {
                /* add to the end if larger than all in the list */
                ++pEntry;
                if (pEntry == &subheap->freelists[LLHEAP_NORMAL_FREE_LISTS])
                    pEntry = subheap->freelists;
                list_add_before( &pEntry->arena.entry, &pFree->entry );
            }
        }
    }
}


/***********************************************************************
 *           LLHEAP_MergeNormalBlock
 */
static LLARENA_NORMAL_BLOCK *CDECL LLHEAP_MergeNormalBlock(LLSUBHEAP *subheap, LLARENA_NORMAL_BLOCK *block)
{
    const LLARENA_NORMAL_BLOCK *limit;
    LLARENA_NORMAL_BLOCK *next;
    LLARENA_NORMAL_BLOCK *prev;
    DWORD blk_size;

    /* merge with next if exists and is free */
    next = get_next_arena(block);
    limit = get_subheap_limit(subheap);
    blk_size = get_blk_size_normal(block);
    if ((next < limit) && (next->flags & LLARENA_FLAG_FREE))
    {
        --subheap->nfree;
        --subheap->nblocks;
        list_remove(&next->entry);
        blk_size += get_blk_size_normal(next);
    }

    /* merge with previous if it is free */
    if ((prev = LLHEAP_GetPrevFreeBlock(block)))
    {
        --subheap->nfree;
        --subheap->nblocks;
        list_remove(&prev->entry);
        blk_size += get_blk_size_normal(prev);
        block = prev;
    }

    set_blk_size_normal(block, blk_size);
    return block;
}

/***********************************************************************
 *           LLHEAP_Decommit
 */
static void CDECL LLHEAP_Decommit(LLARENA_NORMAL_BLOCK *block, SIZE_T blk_size)
{
    ULONG_PTR start;
    ULONG_PTR end;

    /* round address up to nearest page */
    start = (ULONG_PTR)block + sizeof(*block); /* don't unadvise block header */
    start = (start + LLHEAP_PAGE_ALIGNMENT_MASK) & ~LLHEAP_PAGE_ALIGNMENT_MASK;

    /* round size down to nearest page.  make sure that foot is still committed */
    end   = (ULONG_PTR)get_foot(get_next_arena_blk(block, blk_size));
    end  &= ~LLHEAP_PAGE_ALIGNMENT_MASK;

    if (end > start)
        madvise((void*)start, (end - start), MADV_DONTNEED);
}

/***********************************************************************
 *           LLHEAP_CreateFreeBlock
 *
 */
static void CDECL LLHEAP_CreateFreeBlock(LLSUBHEAP *subheap, void *ptr, SIZE_T blk_size)
{
    LLARENA_NORMAL_BLOCK *pFree;

    /* Create a free arena */
    pFree = ptr;
    ++subheap->nfree;
    ++subheap->nblocks;

    /* Insert the new block into the free list */

    set_blk_size_offset(pFree, blk_size, get_subheap_offset(subheap, pFree));
    pFree->flags = LLARENA_FLAG_FREE | LLARENA_FLAG_NORMAL;

    if (blk_size >= LLHEAP_DECOMMIT_THRESHOLD)
        LLHEAP_Decommit(pFree, blk_size);
    LLHEAP_InsertBlockIntoFreeLists(subheap, pFree);
}


/***********************************************************************
 *           LLHEAP_FinishBlock
 */
static inline void *CDECL LLHEAP_FinishBlock(void *inuse, SIZE_T blk_size, SIZE_T req_size)
{
    LLARENA_HEADER *header;
    void *user_ptr;

    header = inuse;
    user_ptr = arena_to_user_ptr(header);
    header->flags &= ~LLARENA_FLAG_FREE;
    header->unused = blk_to_rnd_size(blk_size) - req_size;
    return user_ptr;
}


/***********************************************************************
 *           LLHEAP_CreateSubHeap
 */
static LLSUBHEAP *CDECL LLHEAP_CreateSubHeap(LLHEAP *heap, LPVOID address, DWORD flags, SIZE_T commitSize, SIZE_T totalSize)
{
    LLSUBHEAP *subheap;
    SIZE_T headerSize = 0;
    ULONG_PTR spin = 0;
    int i;

    if (!address)
    {
        if (!commitSize) commitSize = LLCOMMIT_SIZE;
        if (totalSize < commitSize) totalSize = commitSize;
        if (flags & HEAP_SHARED) commitSize = totalSize;  /* always commit everything in a shared heap */
        commitSize = min( totalSize, LLCOMMIT_ALIGN(commitSize) );

        /* allocate the memory block */
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &totalSize,
                                     MEM_RESERVE, get_protection_type( flags ) ))
        {
            WARN("%u:%lu: Could not allocate %08lx bytes\n", getpid(), pthread_self(), totalSize );
            return NULL;
        }
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0,
                                     &commitSize, MEM_COMMIT, get_protection_type( flags ) ))
        {
            WARN("%u:%lu: Could not commit %08lx bytes for sub-heap %p\n", getpid(), pthread_self(), commitSize, address );
            return NULL;
        }
    }

    headerSize = LLSUBHEAP_HEADER_SIZE;
    if (!heap)
    {
        headerSize = LLHEAP_HEADER_SIZE;

        heap = address;
        heap->active            = &heap->subheap;
        heap->obfuscator        = LLHEAP_GetObfuscator(heap);
        heap->tls_idx           = TLS_OUT_OF_INDEXES;
        heap->cs.DebugInfo      = &heap->cs_dbg;
        heap->cs.LockCount      = -1;
        heap->cs.RecursionCount = 0;
        heap->cs.OwningThread   = 0;
        heap->cs.LockSemaphore  = 0;
        heap->cs.SpinCount      = spin;

        zero_memory(&heap->cs_dbg, sizeof(heap->cs_dbg));
        heap->cs_dbg.ProcessLocksList.Flink = &heap->cs_dbg.ProcessLocksList;
        heap->cs_dbg.ProcessLocksList.Blink = &heap->cs_dbg.ProcessLocksList;
        heap->cs_dbg.Spare[0] = (DWORD_PTR)heap->cs_name;
        snprintf(heap->cs_name, sizeof(heap->cs_name), "heap %p", heap);

        heap->magic        = LLHEAP_MAGIC;
        heap->flags        = flags;
        heap->force_flags  = 0;
        heap->grow_size    = LLHEAP_DEF_SUBHEAP_GROW;

        list_init( &heap->subheaps );
        list_init( &heap->entry );
        list_init( &heap->large_list );
        list_init( &heap->perthreads );
        list_init( &heap->free_pts );
        list_init( &heap->free_bufs );
        list_init( &heap->decommit_bufs );
    }

    subheap             = address;
    subheap->nfree      = 0;
    subheap->nblocks    = 0;
    subheap->heap       = heap;
    subheap->size       = totalSize;
    subheap->min_commit = commitSize;
    subheap->commitSize = commitSize;
    subheap->magic      = LLSUBHEAP_MAGIC;
    subheap->headerSize = headerSize;

    zero_memory(subheap->freelists, sizeof(subheap->freelists));
    subheap->freelists[0].arena.flags |= LLARENA_FLAG_FREE;
    list_init(&subheap->freelists[0].arena.entry);
    for (i = 1; i < LLHEAP_NORMAL_FREE_LISTS; i++)
    {
        subheap->freelists[i].arena.flags |= LLARENA_FLAG_FREE;
        list_add_after(&subheap->freelists[i-1].arena.entry,
                       &subheap->freelists[i].arena.entry);
    }

    list_init(&subheap->recent);
    list_add_head(&heap->subheaps, &subheap->entry);

    LLHEAP_CreateFreeBlock(subheap, get_first_block(subheap),
                           subheap->size - subheap->headerSize);

    return subheap;
}


/***********************************************************************
 *           RtlDestroyLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
 *
 * Destroy a Heap created with RtlCreateLLHeap().
 *
 * PARAMS
 *  heap [I] Heap to destroy.
 *
 * RETURNS
 *  Success: A NULL HANDLE, if heap is NULL or it was destroyed
 *  Failure: The Heap handle, if heap is the process heap.
 */
HANDLE WINAPI RtlDestroyHeap( HANDLE handle )
{
    LLARENA_LARGE *arena, *arena_next;
    LLSUBHEAP *subheap;
    struct list *head;
    LLHEAP *heap;
    SIZE_T size;
    void *addr;

    if (LLHEAP_IsOrigHeap(handle))
        return RtlDestroyHeapOrig(handle);

    heap = LLHEAP_GetPtr( handle );

    if (!heap) return handle;

    /* cannot delete the main process heap */
    if (heap == processLLHeap) return handle;

    /* remove it from the per-process list */
    RtlEnterCriticalSection( &processLLHeap->cs );
    list_remove( &heap->entry );
    RtlLeaveCriticalSection( &processLLHeap->cs );

    LLHEAP_TlsFree( heap->tls_idx );
    heap->cs.DebugInfo = NULL;
    RtlDeleteCriticalSection( &heap->cs );

    LIST_FOR_EACH_ENTRY_SAFE( arena, arena_next, &heap->large_list, LLARENA_LARGE, entry )
    {
        list_remove( &arena->entry );
        size = 0;
        addr = arena;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }

    /* remove heap so we can free it last */
    list_remove(&heap->subheap.entry);

    while ((head = list_head(&heap->subheaps)))
    {
        list_remove(head);
        subheap = LIST_ENTRY(head, LLSUBHEAP, entry);

        size = 0;
        addr = subheap;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }

    /* finally free heap struct */
    size = 0;
    addr = heap;
    NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );

    return 0;
}


/***********************************************************************
 *           LLHEAP_AllocateLargeBlock
 */
static void *CDECL LLHEAP_AllocateLargeBlock(LLHEAP *heap, DWORD flags, SIZE_T req_size)
{
    LLARENA_LARGE *arena;
    SIZE_T blk_size;
    SIZE_T unused;
    LPVOID address;
    void *user_ptr;

    address  = NULL;
    blk_size = calc_blk_size_large(flags, req_size);
    if (blk_size < req_size) return NULL;  /* overflow */
    if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 5,
                                 &blk_size, MEM_COMMIT, get_protection_type( flags ) ))
    {
        WARN("%u:%lu: Could not allocate block for %08lx bytes\n", getpid(), pthread_self(), req_size );
        return NULL;
    }

    unused = blk_size - req_size - sizeof(*arena);
    arena = address;
    arena->act_size = req_size;
    arena->blk_size = blk_size;
    zero_memory(arena->pad,  sizeof(arena->pad));
    arena->hdr.dummy  = blk_size; /* may wrap, but ok since used for encoding */
    arena->hdr.flags  = LLARENA_FLAG_LARGE;
    arena->hdr.unused = unused; /* may truncate, but ok since used for encoding */
    arena->hdr.encoding = LLHEAP_Encode(heap, &arena->hdr);

    user_ptr = arena + 1;

    list_add_tail( &heap->large_list, &arena->entry );

    if (flags & HEAP_TAIL_CHECKING_ENABLED)
        LLHEAP_MarkTail(arena, blk_size, unused);

    return user_ptr;
}



/***********************************************************************
 *           LLHEAP_DecommitSubheap
 */
static void CDECL LLHEAP_DecommitSubheap(LLSUBHEAP *subheap)
{
    LLHEAP *heap;
    SIZE_T size;
    LPVOID addr;

    /* remove subheap and replace if active
       there will always be at least one subheap - the heap itself */
    heap = subheap->heap;
    list_remove(&subheap->entry);
    if (heap->active == subheap)
        heap->active = LIST_ENTRY(list_head(&heap->subheaps), LLSUBHEAP, entry);

    size = 0;
    addr = subheap;
    NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
}

/***********************************************************************
 *           LLHEAP_SplitBlock
 */
static void CDECL LLHEAP_SplitBlock(LLSUBHEAP *subheap, LLARENA_NORMAL_BLOCK *pFree, SIZE_T new_blk_size)
{
    LLARENA_NORMAL_BLOCK *pRemainder;
    SIZE_T old_blk_size;

    old_blk_size = get_blk_size_normal(pFree);
    if (old_blk_size >= new_blk_size + LLHEAP_MIN_SPLIT_SIZE)
    {
        set_blk_size_normal(pFree, new_blk_size);
        pRemainder      = get_next_arena_blk(pFree, new_blk_size);
        LLHEAP_CreateFreeBlock(subheap, pRemainder, old_blk_size - new_blk_size);
    }
    else
        LLHEAP_UnsetPrevFree(pFree, get_subheap_limit(subheap));
}


/***********************************************************************
 *           LLHEAP_Commit
 */
static BOOL CDECL LLHEAP_Commit(LLSUBHEAP *subheap, void *pArena, SIZE_T blk_size)
{
    void *ptr;
    SIZE_T size;

    ptr  = get_addr(get_next_arena_blk(pArena, blk_size), sizeof(LLARENA_NORMAL_BLOCK));
    size = get_offset(subheap, ptr);
    size = LLCOMMIT_ALIGN(size);

    if (size > subheap->size) size = subheap->size;
    if (size <= subheap->commitSize) return TRUE;
    size -= subheap->commitSize;
    ptr  = get_addr(subheap, subheap->commitSize);
    if (NtAllocateVirtualMemory( NtCurrentProcess(), &ptr, 0,
                                 &size, MEM_COMMIT, get_protection_type( subheap->heap->flags ) ))
    {
        WARN("(%u:%lu: Heap %p: could not commit %08lx bytes at %p\n",
            getpid(), pthread_self(), subheap->heap, size, ptr);
        return FALSE;
    }
    subheap->commitSize += size;
    return TRUE;
}


/***********************************************************************
 *           LLHEAP_FindBlockInSubheap
 */
static void *CDECL LLHEAP_FindBlockInSubheap(LLSUBHEAP *subheap, SIZE_T blk_size)
{
    LLARENA_NORMAL_BLOCK *pFree;
    LLFREE_LIST_ENTRY *pEntry;
    struct list *cur;
    LLHEAP *heap;
    int idx;

    heap = subheap->heap;
    while ((cur = list_head(&subheap->recent)))
    {
        list_remove(cur);
        pFree = LIST_ENTRY(cur, LLARENA_NORMAL_BLOCK, entry);
        if (get_blk_size_normal(pFree) == blk_size)
        {
            if ((heap->flags & HEAP_VALIDATE) &&
                !LLHEAP_ValidateNormalBlock(heap, pFree, FALSE)) /* recent blocks aren't 'free' */
                return NULL;

            --subheap->nfree;
            return pFree;
        }

        pFree = LLHEAP_MergeNormalBlock(subheap, pFree);
        LLHEAP_InsertBlockIntoFreeLists(subheap, pFree);
    }

    idx     = get_normal_list_index(blk_size);
    pEntry  = &subheap->freelists[idx];
    cur     = &pEntry->arena.entry;
    while ((cur = list_next(&subheap->freelists[0].arena.entry, cur)))
    {
        pFree = LIST_ENTRY(cur, LLARENA_NORMAL_BLOCK, entry);

        if (get_blk_size_normal(pFree) >= blk_size)
        {
            if ((heap->flags & HEAP_VALIDATE) &&
                !LLHEAP_ValidateNormalBlock(heap, pFree, TRUE))
                return NULL;

            if (LLHEAP_Commit(subheap, pFree, blk_size))
            {
                list_remove(cur);
                LLHEAP_SplitBlock(subheap, pFree, blk_size);
                --subheap->nfree;
                return pFree;
            }
        }
    }

    return NULL;
}

/***********************************************************************
 *           LLHEAP_FindFreeBlock
 */
static LLARENA_NORMAL_BLOCK *CDECL LLHEAP_FindFreeBlock(LLHEAP *heap, SIZE_T blk_size)
{
    LLARENA_NORMAL_BLOCK *pFree;
    LLSUBHEAP *subheap2;
    LLSUBHEAP *subheap;

    /* there is always an active subheap - at least the heap itself */
    if ((pFree = LLHEAP_FindBlockInSubheap(heap->active, blk_size)))
        return pFree;

    /* search in other subheaps */
    LIST_FOR_EACH_ENTRY_SAFE(subheap, subheap2, &heap->subheaps, LLSUBHEAP, entry)
    {
        if (heap->active == subheap)
            continue;

        if ((pFree = LLHEAP_FindBlockInSubheap(subheap, blk_size)))
        {
            /* make this subheap active */
            heap->active = subheap;
            return pFree;
        }
    }

    /* next heap to be larger */
    heap->grow_size <<= 1;
    if (heap->grow_size > LLHEAP_MAX_SUBHEAP_GROW)
        heap->grow_size = LLHEAP_MAX_SUBHEAP_GROW;

    if (!(subheap = LLHEAP_CreateSubHeap(heap, NULL, heap->flags, blk_size, heap->grow_size)))
        return NULL;

    /* make active and first search choice */
    heap->active = subheap;

    /* we're guaranteed a block on a freshly created subheap */
    pFree = get_first_block(subheap);
    if (LLHEAP_Commit(subheap, pFree, blk_size))
    {
        list_remove(&pFree->entry);
        --subheap->nfree;
        LLHEAP_SplitBlock(subheap, pFree, blk_size);
        return pFree;
    }

    LLHEAP_DecommitSubheap(subheap);
    return NULL;
}


/***********************************************************************
 *           RtlCreateLLHeap   (NTDLL.@)
 *
 * Locks needed in:
 * Locks held temp:
 * Locks on return:
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
    LLSUBHEAP *subheap;
    DWORD tls_idx;
    LLHEAP *heap;

    if (LLHEAP_UseOrigHeap(addr, flags, totalSize))
        return RtlCreateHeapOrig(flags, addr, totalSize, commitSize, unknown, definition);

    /* fall-back if we can't get a TLS slot */
    if ((tls_idx = LLHEAP_TlsAlloc()) == TLS_OUT_OF_INDEXES)
        return RtlCreateHeapOrig(flags, addr, totalSize, commitSize, unknown, definition);

    /* Allocate the heap block */

    if (!totalSize)
    {
        totalSize = LLHEAP_DEF_SUBHEAP_SIZE;
        flags |= HEAP_GROWABLE;
    }

    if (!(subheap = LLHEAP_CreateSubHeap( NULL, addr, flags, commitSize, totalSize )))
    {
        LLHEAP_TlsFree(tls_idx);
        return 0;
    }
    heap = subheap->heap;
    heap->tls_idx = tls_idx;

    /* Initialize critical section */

    if (flags & HEAP_SHARED)
    {
        /* let's assume that only one thread at a time will try to do this */
        HANDLE sem = heap->cs.LockSemaphore;
        if (!sem) NtCreateSemaphore( &sem, SEMAPHORE_ALL_ACCESS, NULL, 0, 1 );

        NtDuplicateObject( NtCurrentProcess(), sem, NtCurrentProcess(), &sem, 0, 0,
                           DUP_HANDLE_MAKE_GLOBAL | DUP_HANDLE_SAME_ACCESS | DUP_HANDLE_CLOSE_SOURCE );
        heap->cs.LockSemaphore = sem;
        RtlFreeHeap( processLLHeap, 0, heap->cs.DebugInfo );
        heap->cs.DebugInfo = NULL;
    }

    heap_set_debug_flags( heap );

    /* link it into the per-process heap list */
    if (processLLHeap)
    {
        RtlEnterCriticalSection( &processLLHeap->cs );
        list_add_head( &processLLHeap->entry, &heap->entry );
        RtlLeaveCriticalSection( &processLLHeap->cs );
    }
    else if (!addr)
    {
        processLLHeap = heap;  /* assume the first heap we create is the process main heap */
        list_init( &processLLHeap->entry );
        /* one-time env checks */
    }

    return heap;
}




/***********************************************************************
 *           LLHEAP_AllocateNormalBlock
 */
static void *CDECL LLHEAP_AllocateNormalBlock(LLHEAP *heap, DWORD flags, SIZE_T req_size, SIZE_T blk_size)
{
    LLARENA_NORMAL_BLOCK *pFree;
    void *user_ptr;

    user_ptr = NULL;
    if ((pFree = LLHEAP_FindFreeBlock(heap, blk_size)))
    {
        blk_size = get_blk_size_normal(pFree); /* actual block may be larger if too small to split */
        user_ptr = LLHEAP_FinishBlock(pFree, blk_size, req_size);

        LLHEAP_initialize_block( user_ptr, req_size, flags );

        pFree->encoding = LLHEAP_Encode(heap, pFree);

        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(pFree, blk_size, pFree->unused);
    }

    return user_ptr;
}


/***********************************************************************
 *           LLHEAP_FreeNormalBlock
 */
static BOOLEAN CDECL LLHEAP_FreeNormalBlock(LLHEAP *heap, void *ptr)
{
    LLARENA_NORMAL_BLOCK *pFree;
    LLSUBHEAP *subheap;

    pFree   = user_ptr_to_arena(ptr);
    subheap = get_arena_subheap(pFree);

    if (++subheap->nfree == subheap->nblocks)
    {
        if ((LLHEAP *)subheap != heap)
            LLHEAP_DecommitSubheap(subheap);
    }
    else
    {
        pFree->unused = 0;
        pFree->encoding = LLHEAP_Encode(subheap->heap, pFree);
        list_add_head(&subheap->recent, &pFree->entry);
    }

    return TRUE;
}

/***********************************************************************
 *           LLHEAP_ReAllocateNormalBlock
 */
static void *CDECL LLHEAP_ReAllocateNormalBlock(LLHEAP *heap, DWORD flags,
                                                LLARENA_NORMAL_BLOCK *pArena,
                                                SIZE_T req_size, SIZE_T new_blk_size)
{
    SIZE_T old_blk_size;
    SIZE_T old_rnd_size;
    SIZE_T old_act_size;
    LLSUBHEAP *subheap;
    void *old_user_ptr;
    void *ret;

    old_blk_size = get_blk_size_normal(pArena);
    old_rnd_size = blk_to_rnd_size(old_blk_size);
    old_act_size = old_rnd_size - pArena->unused;

    if (new_blk_size == old_blk_size)
    {
        /* new block size is same as old.  just adjust the unused size */
        pArena->unused = old_rnd_size - req_size;
        pArena->encoding = LLHEAP_Encode(heap, pArena);
        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(pArena, new_blk_size, pArena->unused);

        ret = arena_to_user_ptr(pArena);
    }
    else if (new_blk_size < old_blk_size)
    {
        /* new block is smaller.  split and adjust the unused */
        subheap = get_arena_subheap(pArena);
        LLHEAP_SplitBlock(subheap, pArena, new_blk_size);

        /* we can end up with a larger block than requested if
           not enough leftover room to split */
        new_blk_size = get_blk_size_normal(pArena);
        pArena->unused = blk_to_rnd_size(new_blk_size) - req_size;
        pArena->encoding = LLHEAP_Encode(heap, pArena);
        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(pArena, new_blk_size, pArena->unused);

        ret = arena_to_user_ptr(pArena);
    }
    else if (!(flags & HEAP_REALLOC_IN_PLACE_ONLY))
    {
        /* allocate new block and copy */
        if (LLHEAP_BlockTypeFromSize(new_blk_size) == LLARENA_FLAG_LARGE)
            ret = LLHEAP_AllocateLargeBlock(heap, flags, req_size);
        else
            ret = LLHEAP_AllocateNormalBlock(heap, flags, req_size, new_blk_size);

        if (ret)
        {
            old_user_ptr = arena_to_user_ptr(pArena);
            copy_memory(ret, old_user_ptr, min(req_size, old_act_size));
            LLHEAP_FreeNormalBlock(heap, old_user_ptr);
        }
    } else
        ret = NULL;

    /* if we have a block and the new size is larger, initialize the new part */
    if (ret && (req_size > old_act_size))
        LLHEAP_initialize_block((char *)ret + old_act_size, req_size - old_act_size, flags);

    return ret;
}


/***********************************************************************
 *           LLHEAP_FreeLargeBlock
 */
static BOOLEAN CDECL LLHEAP_FreeLargeBlock(LLHEAP *heap, DWORD flags, void *ptr)
{
    LLARENA_LARGE *arena;
    LPVOID address;
    SIZE_T size;

    arena = (LLARENA_LARGE *)ptr - 1;
    address = arena;
    size = 0;

    list_remove( &arena->entry );

    NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
    return TRUE;
}


/***********************************************************************
 *           LLHEAP_ReAllocateLargeBlock
 */
static void *CDECL LLHEAP_ReAllocateLargeBlock(LLHEAP *heap, DWORD flags, void *ptr, SIZE_T req_size)
{
    LLARENA_LARGE *arena;
    SIZE_T unused;
    void *new_ptr;

    arena = (LLARENA_LARGE *)ptr - 1;

    /* if shrinking... */
    if (arena->blk_size - sizeof(*arena) >= req_size)
    {
        unused = arena->blk_size - req_size - sizeof(*arena);
        arena->hdr.unused   = (WORD)unused;
        arena->hdr.encoding = LLHEAP_Encode(heap, &arena->hdr);

        if ((flags & HEAP_TAIL_CHECKING_ENABLED) && (arena->act_size > req_size))
            LLHEAP_MarkTail(arena, arena->blk_size, unused);

        /* FIXME: we could remap zero-pages instead */
        if (req_size > arena->act_size)
            LLHEAP_initialize_block( (char *)ptr + arena->act_size, req_size - arena->act_size, flags );
        arena->act_size = req_size;
        return ptr;
    }

    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return NULL;
    if (!(new_ptr = LLHEAP_AllocateLargeBlock( heap, flags, req_size )))
        return NULL;
    copy_memory( new_ptr, ptr, arena->act_size );
    LLHEAP_FreeLargeBlock( heap, flags, ptr );
    return new_ptr;
}

/***********************************************************************
 *           LLHEAP_InitGroups
 */
static void CDECL LLHEAP_InitGroups(LLPERTHREAD *perthread)
{
    LLGROUP *group;
    int i;

    for (i = 0; i < LLHEAP_NUM_GROUPS; i++)
    {
        group = &perthread->groups[i];
        group->blk_size  = get_group_blk_size(i);
        group->curclr    = NULL;
        group->perthread = perthread;
        list_init(&group->clusters);
        list_init(&group->full);
    }
}

/***********************************************************************
 *           LLHEAP_AllocatePerThread
 */
static LLPERTHREAD *CDECL LLHEAP_AllocatePerThread(LLHEAP *heap)
{
    LLPERTHREAD *perthread;
    struct list *head;
    SIZE_T blk_size;
    SIZE_T size;

    /* a thread may have allocated memory but not freed it by the time it exited
       we save the per-threads in a list to recycle before allocating new ones */
    RtlEnterCriticalSection(&heap->cs);
    if ((head = list_head(&heap->free_pts)))
    {
        list_remove(head);
        perthread = LIST_ENTRY(head, LLPERTHREAD, entry);
        perthread->owner = GetCurrentThreadId();

        list_add_tail(&heap->perthreads, &perthread->entry);
    }
    else
    {
        size = sizeof(*perthread);
        blk_size = calc_blk_size_normal(heap->flags, size);
        if ((perthread = LLHEAP_AllocateNormalBlock(heap, heap->flags, size, blk_size)))
        {
            perthread->owner     = GetCurrentThreadId();
            perthread->fastother = NULL;
            perthread->curbuf    = NULL;
            perthread->heap      = heap;
            list_init(&perthread->buffers);
            LLHEAP_InitGroups(perthread);

            list_add_tail(&heap->perthreads, &perthread->entry);
        }
    }
    RtlLeaveCriticalSection(&heap->cs);

    if (perthread) LLHEAP_TlsSetValue(heap->tls_idx, perthread);
    return perthread;
}

/***********************************************************************
 *           LLHEAP_MergeThreadBlock
 */
static LLARENA_THREAD_BLOCK *CDECL LLHEAP_MergeThreadBlock(LLBUFFER *buffer, LLARENA_THREAD_BLOCK *block)
{
    LLARENA_THREAD_BLOCK *next;
    LLARENA_THREAD_BLOCK *prev;
    DWORD blk_size;

    /* merge with next if exists and is free */
    blk_size = get_blk_size_thread(block);
    next = get_next_arena_blk(block, blk_size);
    if (!is_past_buffer(buffer, next) && (next->flags & LLARENA_FLAG_FREE))
    {
        --buffer->nfree;
        --buffer->nblocks;
        list_remove(&next->entry);
        blk_size += get_blk_size_thread(next);
    }

    /* merge with previous if it is free */
    if ((prev = LLHEAP_GetPrevFreeBlock(block)))
    {
        --buffer->nfree;
        --buffer->nblocks;
        list_remove(&prev->entry);
        blk_size += get_blk_size_thread(prev);
        block = prev;
    }

    set_blk_size_thread(block, blk_size);
    return block;
}

/***********************************************************************
 *           LLHEAP_InsertThreadBlockIntoFreeLists
 */
static void CDECL LLHEAP_InsertThreadBlockIntoFreeLists(LLBUFFER *buffer, LLARENA_THREAD_BLOCK *block)
{
    LLARENA_THREAD_BLOCK *next;
    LLARENA_NORMAL_FOOT *foot;
    DWORD blk_size;
    int idx;

    block->flags |= LLARENA_FLAG_FREE;
    block->encoding = LLHEAP_Encode(buffer->perthread->heap, block);

    blk_size = get_blk_size_thread(block);
    idx = get_thread_list_index(blk_size);
    set_free_idx(buffer->freemap, idx);
    list_add_head(&buffer->freelists[idx], &block->entry);

    /* because of the shifting, the block size is truncated
       so, the next block may not touch limit */
    next = get_next_arena_blk(block, blk_size);
    if (!is_past_buffer(buffer, next))
    {
        /* notify next we're free */
        next->flags |= LLARENA_FLAG_PREV_FREE;
        foot = get_foot(next);
        foot->pPrevFree = block;
    }
}

/***********************************************************************
 *           LLHEAP_DecommitBuffer
 */
static void CDECL LLHEAP_DecommitBuffer(LLBUFFER *buffer)
{
    ULONG_PTR start;
    ULONG_PTR end;

    /* round address up to nearest page */
    start = (ULONG_PTR)buffer + LLBUFFER_HEADER_SIZE;
    start = (start + LLHEAP_PAGE_ALIGNMENT_MASK) & ~LLHEAP_PAGE_ALIGNMENT_MASK;

    /* round size down to nearest page.  make sure that foot is still committed */
    end   = (ULONG_PTR)buffer + LLHEAP_BUFFER_SIZE;
    end  &= ~LLHEAP_PAGE_ALIGNMENT_MASK;

    if (end > start)
        madvise((void*)start, (end - start), MADV_DONTNEED);
}

/***********************************************************************
 *           LLHEAP_DecommitFreedBuffers
 */
static void CDECL LLHEAP_DecommitFreedBuffers(LLHEAP *heap)
{
    struct list *head;
    LLBUFFER *buffer;

    while ((head = list_head(&heap->free_bufs)))
    {
        buffer = LIST_ENTRY(head, LLBUFFER, entry);
        list_remove(head);
        list_add_tail(&heap->decommit_bufs, &buffer->entry);
        LLHEAP_DecommitBuffer(buffer);
    }
}

/***********************************************************************
 *           LLHEAP_PushFreeThreadBlock
 */
static BOOL CDECL LLHEAP_PushFreeThreadBlock(LLARENA_THREAD_BLOCK *block)
{
    LLPERTHREAD *perthread;
    struct list *head;
    LLBUFFER *bufhead;
    LLBUFFER *buffer;
    LLHEAP *heap;
    time_t now;

    buffer = get_buffer(block);
    perthread = buffer->perthread;
    heap = perthread->heap;

    /* if this is the last block, free the buffer */
    if (++buffer->nfree == buffer->nblocks)
    {
        list_remove(&buffer->entry);

        /* set a new active if we were it */
        if (perthread->curbuf == buffer)
        {
            if ((head = list_head(&perthread->buffers)))
                perthread->curbuf = LIST_ENTRY(head, LLBUFFER, entry);
            else
                perthread->curbuf = NULL;
        }

        now = current_time();
        RtlEnterCriticalSection(&heap->cs);
        if ((head = list_head(&heap->free_bufs)))
        {
            bufhead = LIST_ENTRY(head, LLBUFFER, entry);
            if ((now - bufhead->tmfreed) > LLHEAP_FREE_BUFFER_WAIT)
                LLHEAP_DecommitFreedBuffers(heap);
        }
        buffer->tmfreed = now;
        list_add_head(&heap->free_bufs, &buffer->entry);
        RtlLeaveCriticalSection(&heap->cs);

        return FALSE;
    }
    else
    {
        block->unused   = 0;
        block->encoding = LLHEAP_Encode(heap, block);

        list_add_head(&buffer->recent, &block->entry);
        /* if "half" empty, make first in the search list
           this keeps a buffer on its way to being freed active
           avoiding the relatively costly decommitting.  it'll
           still be freed if all blocks are returned without
           another allocation */
        if ((buffer->nfree << 1) >= buffer->nblocks)
        {
            list_remove(&buffer->entry);
            list_add_head(&perthread->buffers, &buffer->entry);
        }

        return TRUE;
    }
}

/***********************************************************************
 *           LLHEAP_PushOtherThreadBlocks
 */
static BOOL CDECL LLHEAP_PushOtherThreadBlocks(LLBUFFER *buffer)
{
    void *entry;
    LLARENA_THREAD_BLOCK *block;

    /* only make interlocked call if we have blocks from other threads */
    if (have_other_thread_blocks(&buffer->other))
    {
        if ((entry = flush_thread_blocks(&buffer->other)))
        {
            while (entry)
            {
                block = user_ptr_to_arena(entry);
                entry = *(void **)entry; /* get next pointer before overwriting by free */
                if (!LLHEAP_PushFreeThreadBlock(block))
                    return FALSE; /* pushed last block, buffer is freed */
            }
        }
    }

    return TRUE;
}

/***********************************************************************
 *           LLHEAP_CreateFreeThreadBlock
 */
static void CDECL LLHEAP_CreateFreeThreadBlock(LLBUFFER *buffer, void *ptr, SIZE_T blk_size)
{
    LLARENA_THREAD_BLOCK *block;
    LLHEAP *heap;

    ++buffer->nfree;
    ++buffer->nblocks;
    heap = buffer->perthread->heap;

    block = ptr;
    set_blk_size_thread(block, blk_size);
    block->flags    = LLARENA_FLAG_THREAD;
    block->offset   = get_buffer_offset(buffer, block);
    block->unused   = 0;
    block->encoding = LLHEAP_Encode(heap, block);

    LLHEAP_InsertThreadBlockIntoFreeLists(buffer, block);
}

/***********************************************************************
 *           LLHEAP_SplitThreadBlock
 */
static void CDECL LLHEAP_SplitThreadBlock(LLBUFFER *buffer, LLARENA_THREAD_BLOCK *block, SIZE_T new_blk_size)
{
    LLARENA_THREAD_BLOCK *remain;
    LLARENA_THREAD_BLOCK *next;
    SIZE_T old_blk_size;

    old_blk_size = get_blk_size_thread(block);
    if (old_blk_size >= new_blk_size + LLHEAP_MIN_SPLIT_SIZE)
    {
        set_blk_size_thread(block, new_blk_size);
        remain = get_next_arena_blk(block, new_blk_size);
        LLHEAP_CreateFreeThreadBlock(buffer, remain, old_blk_size - new_blk_size);
    }
    else
    {
        next = get_next_arena_blk(block, old_blk_size);
        if (!is_past_buffer(buffer, next))
            next->flags &= ~LLARENA_FLAG_PREV_FREE;
    }
}

/***********************************************************************
 *           LLHEAP_FindThreadBlockInBuffer
 */
static LLARENA_THREAD_BLOCK *CDECL LLHEAP_FindThreadBlockInBuffer(LLBUFFER *buffer, SIZE_T blk_size)
{
    int bi;
    int bmi;
    int idx;
    DWORD mask;
    DWORD bitmap;
    LLHEAP *heap;
    struct list *cur;
    LLARENA_THREAD_BLOCK *block;

    /* try from recent first */
    heap = buffer->perthread->heap;
    while ((cur = list_head(&buffer->recent)))
    {
        list_remove(cur);
        block = LIST_ENTRY(cur, LLARENA_THREAD_BLOCK, entry);
        if (get_blk_size_thread(block) == blk_size)
        {
            if ((heap->flags & HEAP_VALIDATE) &&
                !LLHEAP_ValidateThreadBlock(heap, block, FALSE)) /* recent blocks aren't 'free' */
                return NULL;
            --buffer->nfree;
            return block;
        }

        block = LLHEAP_MergeThreadBlock(buffer, block);
        LLHEAP_InsertThreadBlockIntoFreeLists(buffer, block);
    }

    /* get from same size or larger */
    idx  = get_thread_list_index(blk_size);
    bmi  = idx >> LLHEAP_LIST_BITMAP_SHIFT;
    bitmap = buffer->freemap[bmi];
    /* bitmap different for first iteration */
    bitmap &= ~((1 << (idx & LLHEAP_LIST_BITMAP_MASK)) - 1);
    for (;;)
    {
        while ((bi = ffs(bitmap)))
        {
            --bi;
            idx = (bmi << LLHEAP_LIST_BITMAP_SHIFT) + bi;
            LIST_FOR_EACH_ENTRY(block, &buffer->freelists[idx], LLARENA_THREAD_BLOCK, entry)
            {
                if (get_blk_size_thread(block) >= blk_size)
                {
                    if ((heap->flags & HEAP_VALIDATE) &&
                        !LLHEAP_ValidateThreadBlock(heap, block, TRUE))
                        return NULL;

                    list_remove(&block->entry);
                    LLHEAP_SplitThreadBlock(buffer, block, blk_size);
                    --buffer->nfree;
                    return block;
                }
            }

            mask = ~(1 << bi);
            /* no blocks of right size were found
               if there weren't any at all, unset the hint */
            if (list_empty(&buffer->freelists[idx]))
                buffer->freemap[bmi] &= mask;

            bitmap &= mask;
        }

        if (++bmi >= LLHEAP_THREAD_LIST_BITMAPS)
            break;

        bitmap = buffer->freemap[bmi];
    }

    return NULL;
}

/***********************************************************************
 *           LLHEAP_AllocateBuffer
 */
static LLBUFFER *CDECL LLHEAP_AllocateBuffer(LLPERTHREAD *perthread)
{
    struct list *head;
    LLBUFFER *buffer;
    SIZE_T blk_size;
    LLHEAP *heap;
    int i;

    heap = perthread->heap;
    blk_size = calc_blk_size_normal(heap->flags, LLHEAP_BUFFER_SIZE);

    RtlEnterCriticalSection(&heap->cs);
    if ((head = list_head(&heap->free_bufs)))
    {
        list_remove(head);
        buffer = LIST_ENTRY(head, LLBUFFER, entry);
    }
    else if ((head = list_head(&heap->decommit_bufs)))
    {
        list_remove(head);
        buffer = LIST_ENTRY(head, LLBUFFER, entry);
    }
    else
        buffer = LLHEAP_AllocateNormalBlock(heap, heap->flags, LLHEAP_BUFFER_SIZE, blk_size);
    RtlLeaveCriticalSection(&heap->cs);

    if (buffer)
    {
        buffer->nfree     = 0;
        buffer->nblocks   = 0;
        buffer->tmfreed   = 0;
        buffer->other     = NULL;
        buffer->perthread = perthread;

        zero_memory(buffer->freemap, sizeof(buffer->freemap));
        list_init(&buffer->recent);
        for (i = 0; i < LLHEAP_THREAD_LISTS; i++)
            list_init(&buffer->freelists[i]);

        LLHEAP_CreateFreeThreadBlock(buffer, get_first_buffer_block(buffer),
                                     LLHEAP_BUFFER_SIZE - LLBUFFER_HEADER_SIZE);
    }

    return buffer;
}

/***********************************************************************
 *           LLHEAP_FindThreadBlock
 */
static LLARENA_THREAD_BLOCK *CDECL LLHEAP_FindThreadBlock(LLPERTHREAD *perthread, SIZE_T blk_size)
{
    LLARENA_THREAD_BLOCK *block;
    LLBUFFER *buffer2;
    LLBUFFER *buffer;

    if (perthread->curbuf &&
        (block = LLHEAP_FindThreadBlockInBuffer(perthread->curbuf, blk_size)))
        return block;

    LIST_FOR_EACH_ENTRY_SAFE(buffer, buffer2, &perthread->buffers, LLBUFFER, entry)
    {
        if (have_other_thread_blocks(&buffer->other))
        {
            /* this could free the buffer and remove if from the buffer list */
            if (!LLHEAP_PushOtherThreadBlocks(buffer))
                continue;
        }

        if ((block = LLHEAP_FindThreadBlockInBuffer(buffer, blk_size)))
        {
            /* make this active and the first in the search list */
            perthread->curbuf = buffer;

            list_remove(&buffer->entry);
            list_add_head(&perthread->buffers, &buffer->entry);
            return block;
        }
    }

    /* allocate new buffer and use first block */
    if (!(buffer = LLHEAP_AllocateBuffer(perthread)))
        return NULL;

    /* make active and first search choice */
    perthread->curbuf = buffer;
    list_add_head(&perthread->buffers, &buffer->entry);

    /* we're guaranteed a block on a freshly created buffer */
    block = get_first_buffer_block(buffer);

    --buffer->nfree;
    list_remove(&block->entry);
    LLHEAP_SplitThreadBlock(buffer, block, blk_size);
    return block;
}

/***********************************************************************
 *           LLHEAP_AllocateThreadBlock
 */
static void *CDECL LLHEAP_AllocateThreadBlock(LLPERTHREAD *perthread, DWORD flags,
                                              SIZE_T req_size, SIZE_T blk_size)
{
    LLARENA_THREAD_BLOCK *block;
    void *user_ptr;

    if ((block = LLHEAP_FindThreadBlock(perthread, blk_size)))
    {
        blk_size = get_blk_size_thread(block); /* can find larger block than requested */
        user_ptr = LLHEAP_FinishBlock(block, blk_size, req_size);

        block->encoding = LLHEAP_Encode(perthread->heap, block);

        LLHEAP_initialize_block(user_ptr, req_size, flags);

        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(block, blk_size, block->unused);

        return user_ptr;
    }

    return NULL;
}

/***********************************************************************
 *           LLHEAP_PurgeBuffers
 */
static void CDECL LLHEAP_PurgeBuffers(LLHEAP *heap, struct list *bufs)
{
    struct list *head;
    LLBUFFER *buffer;

    while ((head = list_head(bufs)))
    {
        buffer = LIST_ENTRY(head, LLBUFFER, entry);
        list_remove(head);
        LLHEAP_FreeNormalBlock(heap, buffer);
    }
}

/***********************************************************************
 *           LLHEAP_FreeThreadBlock
 */
static BOOL CDECL LLHEAP_FreeThreadBlock(LLHEAP *heap, void *ptr)
{
    LLARENA_THREAD_BLOCK *block;
    LLPERTHREAD *perthread;
    LLPERTHREAD *blockpt;
    LLBUFFER *buffer;

    block = user_ptr_to_arena(ptr);
    buffer = get_buffer(block);
    blockpt = buffer->perthread;

    perthread = LLHEAP_GetPerThread(heap);
    if (blockpt != perthread)
    {
        /* push atomically to slist if on another thread
           note that this does nothing with the free block count */
        push_thread_block(&buffer->other, (void**)&block->entry);
        if (blockpt->owner == 0)
        {
            /* no thread owns this perthread, so it won't be cleaned */
            RtlEnterCriticalSection(&heap->cs);
            /* check again in case another thread claims it before
               we grabbed the lock. while we hold the lock, no other
               thread can claim it, so it's safe to free the memory on it */
            if (blockpt->owner == 0)
                LLHEAP_PushOtherThreadBlocks(buffer);
            RtlLeaveCriticalSection(&heap->cs);
        }
        return TRUE;
    }

    LLHEAP_PushFreeThreadBlock(block);
    return TRUE;
}

/***********************************************************************
 *           LLHEAP_PushFastBlock
 */
static void CDECL LLHEAP_PushFastBlock(LLHEAP *heap, LLCLUSTER *cluster, LLARENA_FAST_BLOCK *block)
{
    struct list *head;
    LLGROUP *group;

    block->next = cluster->last;
    cluster->last = block->offset;

    block->flags  = LLARENA_FLAG_FREE | LLARENA_FLAG_FAST;
    block->unused = 0;
    block->encoding = LLHEAP_Encode(heap, block);

    if (!cluster->nfree++)
    {
        /* remove from full list */
        group = cluster->group;
        list_remove(&cluster->entry);
        list_add_head(&group->clusters, &cluster->entry);
    }

    if (cluster->nfree == LLHEAP_NUM_CLUSTER_BLOCKS)
    {
        /* all blocks are free, remove from list */
        list_remove(&cluster->entry);

        /* set new active if we were it */
        group = cluster->group;
        if (group->curclr == cluster)
        {
            if ((head = list_head(&group->clusters)))
                group->curclr = LIST_ENTRY(head, LLCLUSTER, entry);
            else
                group->curclr = NULL;
        }

        LLHEAP_FreeThreadBlock(heap, cluster);
    }
}

/***********************************************************************
 *           LLHEAP_PurgeOtherFastBlocks
 */
static void CDECL LLHEAP_PurgeOtherFastBlocks(LLPERTHREAD *perthread)
{
    void *entry;
    LLHEAP *heap;
    LLARENA_FAST_BLOCK *block;

    /* only make interlocked call if we have blocks from other threads */
    if (have_other_thread_blocks(&perthread->fastother))
    {
        if ((entry = flush_thread_blocks(&perthread->fastother)))
        {
            heap = perthread->heap;
            while (entry)
            {
                block = user_ptr_to_arena(entry);
                entry = *(void **)entry; /* get next pointer before overwriting by free */
                LLHEAP_PushFastBlock(heap, get_cluster(block), block);
            }
        }
    }
}

/***********************************************************************
 *           LLHEAP_FreePerThread
 */
static void CDECL LLHEAP_FreePerThread(LLHEAP *heap, LLPERTHREAD *perthread)
{
    LLBUFFER *buffer2;
    LLBUFFER *buffer;

    LLHEAP_PurgeOtherFastBlocks(perthread); /* purge fast blocks first */

    /* free any blocks from other threads */
    LIST_FOR_EACH_ENTRY_SAFE(buffer, buffer2, &perthread->buffers, LLBUFFER, entry)
    {
        /* this could free the buffer and remove if from the buffer list */
        LLHEAP_PushOtherThreadBlocks(buffer);
    }

    /* remove from list of active per-threads */
    list_remove(&perthread->entry);

    /* if all the buffers in the thread are freed, free the perthread */
    if (list_empty(&perthread->buffers))
        LLHEAP_FreeNormalBlock(heap, perthread);
    else
    {
        /* there are still buffers allocated by this thread
           put the per-thread on the free list to be recycled */
        list_add_head(&heap->free_pts, &perthread->entry);
        perthread->owner = 0;
    }
}

/***********************************************************************
 *           llheap_notify_thread_term
 */
void llheap_notify_thread_term(void)
{
    LLPERTHREAD *perthread;
    LLHEAP *heap;

    if (!processLLHeap)
        return;

    RtlEnterCriticalSection(&processLLHeap->cs);
    if ((perthread = LLHEAP_GetPerThread(processLLHeap)))
        LLHEAP_FreePerThread(processLLHeap, perthread);

    LIST_FOR_EACH_ENTRY(heap, &processLLHeap->entry, LLHEAP, entry)
    {
        RtlEnterCriticalSection(&heap->cs);
        if ((perthread = LLHEAP_GetPerThread(heap)))
            LLHEAP_FreePerThread(heap, perthread);
        RtlLeaveCriticalSection(&heap->cs);
    }
    RtlLeaveCriticalSection(&processLLHeap->cs);
}

/***********************************************************************
 *           LLHEAP_ReAllocateThreadBlock
 */
static void *CDECL LLHEAP_ReAllocateThreadBlock(LLPERTHREAD *perthread, DWORD flags, LLARENA_THREAD_BLOCK *block,
                                                SIZE_T req_size, SIZE_T new_blk_size)
{
    SIZE_T  old_blk_size;
    SIZE_T  old_rnd_size;
    SIZE_T  old_act_size;
    void   *old_user_ptr;
    void   *new_user_ptr;
    LLBUFFER *buffer;
    LLHEAP *heap;

    old_blk_size = get_blk_size_thread(block);
    old_rnd_size = blk_to_rnd_size(old_blk_size);
    old_act_size = old_rnd_size - block->unused;
    old_user_ptr = arena_to_user_ptr(block);
    heap = perthread->heap;

    if (old_blk_size == new_blk_size)
    {
        /* same size - just adjust the unused */
        block->unused = old_rnd_size - req_size;
        block->encoding = LLHEAP_Encode(heap, block);
        new_user_ptr = old_user_ptr;

        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(block, new_blk_size, block->unused);
    }
    else if (new_blk_size < old_blk_size)
    {
        /* new block is smaller.  split and adjust the unused */
        buffer = get_buffer(block);
        LLHEAP_SplitThreadBlock(buffer, block, new_blk_size);

        /* we can end up with a larger block than requested if
           not enough leftover room to split */
        new_blk_size = get_blk_size_thread(block);

        block->unused = blk_to_rnd_size(new_blk_size) - req_size;
        block->encoding = LLHEAP_Encode(heap, block);
        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(block, new_blk_size, block->unused);

        new_user_ptr = old_user_ptr;
    }
    else if (!(flags & HEAP_REALLOC_IN_PLACE_ONLY))
    {
        new_user_ptr = NULL;
        switch (LLHEAP_BlockTypeFromSize(new_blk_size))
        {
            case LLARENA_FLAG_FAST:
                /* fall-through and keep this a thread block */
            case LLARENA_FLAG_THREAD:
                /* allocate new block on own thread */
                new_user_ptr = LLHEAP_AllocateThreadBlock(perthread, flags, req_size, new_blk_size);
                break;
            case LLARENA_FLAG_NORMAL:
                RtlEnterCriticalSection(&heap->cs);
                new_user_ptr = LLHEAP_AllocateNormalBlock(heap, flags, req_size, new_blk_size);
                RtlLeaveCriticalSection(&heap->cs);
                break;
            case LLARENA_FLAG_LARGE:
                RtlEnterCriticalSection(&heap->cs);
                new_user_ptr = LLHEAP_AllocateLargeBlock(heap, flags, req_size);
                RtlLeaveCriticalSection(&heap->cs);
                break;
        }

        if (new_user_ptr)
        {
            copy_memory(new_user_ptr, old_user_ptr, min(req_size, old_act_size));
            LLHEAP_FreeThreadBlock(heap, old_user_ptr);
        }
    } else
        new_user_ptr = NULL;

    if (new_user_ptr)
    {
        if (req_size > old_act_size)
            LLHEAP_initialize_block((char *)new_user_ptr + old_act_size, req_size - old_act_size, flags);
    }
    else
    {
        if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus( STATUS_NO_MEMORY );
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_NO_MEMORY );
    }

    return new_user_ptr;
}

/***********************************************************************
 *           LLHEAP_AllocateCluster
 */
static LLCLUSTER *CDECL LLHEAP_AllocateCluster(LLGROUP *group)
{
    LLCLUSTER *cluster;
    DWORD rnd_size;
    DWORD blk_size;
    LLHEAP *heap;

    heap = group->perthread->heap;
    rnd_size = LLCLUSTER_HEADER_SIZE + LLHEAP_NUM_CLUSTER_BLOCKS * group->blk_size;
    blk_size = calc_blk_size_normal(heap->flags, rnd_size);
    if (!(cluster = LLHEAP_AllocateThreadBlock(group->perthread, 0, rnd_size, blk_size)))
        return NULL;

    cluster->group   = group;
    cluster->last    = 0;
    cluster->nfree   = LLHEAP_NUM_CLUSTER_BLOCKS;
    cluster->top     = LLCLUSTER_HEADER_SIZE;
    cluster->limit   = LLHEAP_NUM_CLUSTER_BLOCKS * group->blk_size;

    /* make this one active */
    group->curclr = cluster;
    list_add_head(&group->clusters, &cluster->entry);

    return cluster;
}

/***********************************************************************
 *           LLHEAP_FindFastBlockInCluster
 */
static LLARENA_FAST_BLOCK *CDECL LLHEAP_FindFastBlockInCluster(LLCLUSTER *cluster)
{
    LLARENA_FAST_BLOCK *block;

    /* pop from last used, if available */
    if (cluster->last)
    {
        block = get_cluster_block(cluster, cluster->last);
        cluster->last = block->next;
        --cluster->nfree;
        block->next = 0;
        return block;
    }
    else if (cluster->top < cluster->limit)
    {
        /* else pop from top, if available */
        block = get_cluster_block(cluster, cluster->top);

        /* fill in block */
        block->offset = cluster->top;
        block->next   = 0;
        block->flags  = LLARENA_FLAG_FAST;

        --cluster->nfree;
        cluster->top += cluster->group->blk_size;

        return block;
    }

    /* else full */
    return NULL;
}

/***********************************************************************
 *           LLHEAP_FindFastBlockInGroup
 */
static LLARENA_FAST_BLOCK *CDECL LLHEAP_FindFastBlockInGroup(LLGROUP *group)
{
    LLARENA_FAST_BLOCK *block;
    LLCLUSTER *cluster;

    /* find fast block in current cluster */
    if ((cluster = group->curclr))
    {
        if ((block = LLHEAP_FindFastBlockInCluster(cluster)))
            return block;

        /* move to full list */
        list_remove(&cluster->entry);
        list_add_head(&group->full, &cluster->entry);
    }

    /* search other clusters */
    LIST_FOR_EACH_ENTRY(cluster, &group->clusters, LLCLUSTER, entry)
    {
        if (cluster == group->curclr)
            continue;

        if ((block = LLHEAP_FindFastBlockInCluster(cluster)))
        {
            /* make this active and the first one in the list */
            group->curclr = cluster;

            list_remove(&cluster->entry);
            list_add_head(&group->clusters, &cluster->entry);
            return block;
        }
    }

    /* allocate new cluster */
    if (!(cluster = LLHEAP_AllocateCluster(group)))
        return NULL;

    /* we're guaranteed a block on a new cluster */
    return LLHEAP_FindFastBlockInCluster(cluster);
}

/***********************************************************************
 *           LLHEAP_AllocateFastBlock
 */
static void *CDECL LLHEAP_AllocateFastBlock(LLPERTHREAD *perthread, DWORD flags,
                                            SIZE_T req_size, SIZE_T blk_size)
{
    LLARENA_FAST_BLOCK *block;
    LLGROUP *group;
    void *user_ptr;

    group = get_group(perthread, blk_size);
    if ((block = LLHEAP_FindFastBlockInGroup(group)))
    {
        user_ptr = LLHEAP_FinishBlock(block, blk_size, req_size);

        block->encoding = LLHEAP_Encode(perthread->heap, block);

        LLHEAP_initialize_block(user_ptr, req_size, flags);

        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(block, blk_size, block->unused);

        return user_ptr;
    }

    return NULL;
}

/***********************************************************************
 *           LLHEAP_FreeFastBlock
 */
static BOOL CDECL LLHEAP_FreeFastBlock(LLHEAP *heap, void *ptr)
{
    LLARENA_FAST_BLOCK *block;
    LLPERTHREAD *perthread;
    LLPERTHREAD *blockpt;
    LLCLUSTER *cluster;

    block   = user_ptr_to_arena(ptr);
    cluster = get_cluster(block);
    blockpt = cluster->group->perthread;

    /* if on another thread, use interlocked push */
    perthread = LLHEAP_GetPerThread(heap);
    if (perthread != blockpt)
    {
        /* push atomically to slist if on another thread
           note that this does nothing with the free block count */
        push_thread_block(&blockpt->fastother, (void**)&block->entry);
        return TRUE;
    }

    /* for blocks in this thread, don't use interlocked push */
    LLHEAP_PushFastBlock(heap, cluster, block);
    return TRUE;
}

/***********************************************************************
 *           LLHEAP_ReAllocateFastBlock
 */
static void *CDECL LLHEAP_ReAllocateFastBlock(LLPERTHREAD *perthread, DWORD flags, LLARENA_FAST_BLOCK *block,
                                              SIZE_T req_size, SIZE_T new_blk_size)
{
    LLHEAP *heap;
    LLGROUP *group;
    LLCLUSTER *cluster;
    SIZE_T old_rnd_size;
    SIZE_T old_act_size;
    void *old_user_ptr;
    void *new_user_ptr;

    cluster = get_cluster(block);
    group   = cluster->group;
    heap    = perthread->heap;

    old_rnd_size = blk_to_rnd_size(group->blk_size);
    old_act_size = old_rnd_size - block->unused;
    old_user_ptr = arena_to_user_ptr(block);

    new_user_ptr = NULL;
    if (group->blk_size == new_blk_size)
    {
        /* same size - just adjust the unused */
        block->unused   = old_rnd_size - req_size;
        block->encoding = LLHEAP_Encode(heap, block);
        new_user_ptr    = old_user_ptr;

        if (flags & HEAP_TAIL_CHECKING_ENABLED)
            LLHEAP_MarkTail(block, new_blk_size, block->unused);
    }
    else if (!(flags & HEAP_REALLOC_IN_PLACE_ONLY))
    {
        /* fast block can't be split, so we have to allocate a new block */
        switch (LLHEAP_BlockTypeFromSize(new_blk_size))
        {
            case LLARENA_FLAG_FAST:
                new_user_ptr = LLHEAP_AllocateFastBlock(perthread, flags, req_size, new_blk_size);
                break;
            case LLARENA_FLAG_THREAD:
                new_user_ptr = LLHEAP_AllocateThreadBlock(perthread, flags, req_size, new_blk_size);
                break;
            case LLARENA_FLAG_NORMAL:
                RtlEnterCriticalSection(&heap->cs);
                new_user_ptr = LLHEAP_AllocateNormalBlock(heap, flags, req_size, new_blk_size);
                RtlLeaveCriticalSection(&heap->cs);
                break;
            case LLARENA_FLAG_LARGE:
                RtlEnterCriticalSection(&heap->cs);
                new_user_ptr = LLHEAP_AllocateLargeBlock(heap, flags, req_size);
                RtlLeaveCriticalSection(&heap->cs);
                break;
        }

        if (new_user_ptr)
        {
            copy_memory(new_user_ptr, old_user_ptr, min(req_size, old_act_size));
            LLHEAP_FreeFastBlock(heap, old_user_ptr);
        }
    }

    return new_user_ptr;
}

/***********************************************************************
 *           RtlAllocateLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp:
 * Locks on return: none
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
PVOID WINAPI RtlAllocateHeap( HANDLE handle, ULONG flags, SIZE_T req_size )
{
    LLPERTHREAD *perthread;
    SIZE_T blk_size;
    LLHEAP *heap;
    DWORD type;
    PVOID ret;

    if (LLHEAP_IsOrigHeap(handle))
        return RtlAllocateHeapOrig(handle, flags, req_size);

    /* Validate the parameters */

    heap = LLHEAP_GetPtr( handle );
    if (!heap) return NULL;
    flags &= HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY;
    flags |= heap->flags;

    blk_size = calc_blk_size_normal(flags, req_size);
    /* check for overflow */
    if (blk_size < req_size)
    {
        if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus( STATUS_NO_MEMORY );
        return NULL;
    }

    if ((perthread = LLHEAP_GetPerThread(heap)))
    {
        if (have_other_thread_blocks(&perthread->fastother))
            LLHEAP_PurgeOtherFastBlocks(perthread);
    }

    ret = NULL;
    type = LLHEAP_BlockTypeFromSize(blk_size);
    if (type == LLARENA_FLAG_FAST)
    {
        /* fast blocks can be smaller than all other blocks */
        blk_size = max(LLHEAP_MIN_FAST_BLOCK_SIZE, blk_size);
        if (perthread || (perthread = LLHEAP_AllocatePerThread(heap)))
            ret = LLHEAP_AllocateFastBlock(perthread, flags, req_size, blk_size);
        if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );
        return ret;
    }

    blk_size = max(LLHEAP_MIN_BLOCK_SIZE, blk_size);
    if (type == LLARENA_FLAG_THREAD)
    {
        /* handle per-thread blocks outside of lock */
        if (perthread || (perthread = LLHEAP_AllocatePerThread(heap)))
            ret = LLHEAP_AllocateThreadBlock(perthread, flags, req_size, blk_size);
        if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );
        return ret;
    }

    RtlEnterCriticalSection(&heap->cs);
    LLHEAP_PurgeBuffers(heap, &heap->free_bufs);
    LLHEAP_PurgeBuffers(heap, &heap->decommit_bufs);

    if (type == LLARENA_FLAG_NORMAL)
        ret = LLHEAP_AllocateNormalBlock( heap, flags, req_size, blk_size );
    else
        ret = LLHEAP_AllocateLargeBlock( heap, flags, req_size ); /* size will be re-rounded for large block */

    RtlLeaveCriticalSection(&heap->cs);
    if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );

    return ret;
}


/***********************************************************************
 *           RtlFreeLLHeap   (NTDLL.@)
 *
 * Locks needed in:
 * Locks held temp:
 * Locks on return:
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
BOOLEAN WINAPI RtlFreeHeap( HANDLE handle, ULONG flags, PVOID ptr )
{
    LLPERTHREAD *perthread;
    const void *pArena;
    LLHEAP *heap;
    BOOLEAN ret;
    DWORD type;

    /* Validate the parameters */

    if (!ptr) return TRUE;  /* freeing a NULL ptr isn't an error in Win2k */

    if (LLHEAP_IsOrigHeap(handle))
        return RtlFreeHeapOrig(handle, flags, ptr);

    heap = LLHEAP_GetPtr( handle );
    if (!heap)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_HANDLE );
        return FALSE;
    }

    flags |= heap->flags;

    if ((perthread = LLHEAP_GetPerThread(heap)))
    {
        if (have_other_thread_blocks(&perthread->fastother))
            LLHEAP_PurgeOtherFastBlocks(perthread);
    }

    ret    = TRUE;
    pArena = user_ptr_to_arena(ptr);
    type   = LLHEAP_BlockType(pArena);

    switch (type)
    {
        case LLARENA_FLAG_FAST:
        {
            if (flags & HEAP_VALIDATE)
                ret = LLHEAP_ValidateFastBlock(heap, pArena, FALSE);
            if (ret)
                ret = LLHEAP_FreeFastBlock(heap, ptr);
            return ret;
        }

        case LLARENA_FLAG_THREAD:
        {
            if (flags & HEAP_VALIDATE)
                ret = LLHEAP_ValidateThreadBlock(heap, pArena, FALSE);
            if (ret)
                ret = LLHEAP_FreeThreadBlock(heap, ptr);
            return ret;
        }
    }

    RtlEnterCriticalSection(&heap->cs);
    LLHEAP_PurgeBuffers(heap, &heap->free_bufs);
    LLHEAP_PurgeBuffers(heap, &heap->decommit_bufs);

    if (flags & HEAP_VALIDATE)
    {
        switch (type)
        {
            case LLARENA_FLAG_NORMAL:
                ret = LLHEAP_ValidateNormalBlock(heap, pArena, FALSE);
                break;
            case LLARENA_FLAG_LARGE:
                ret = LLHEAP_ValidateLargeBlock(heap, ptr);
                break;
            default:
                ret = FALSE;
                break;
        }
    }

    if (ret)
    {
        switch (type)
        {
            case LLARENA_FLAG_NORMAL:
                ret = LLHEAP_FreeNormalBlock(heap, ptr);
                break;
            case LLARENA_FLAG_LARGE:
                ret = LLHEAP_FreeLargeBlock(heap, flags, ptr);
                break;
            default:
                ret = FALSE;
                break;
        }
    }

    RtlLeaveCriticalSection(&heap->cs);

    return ret;
}


/***********************************************************************
 *           RtlReAllocateLLHeap   (NTDLL.@)
 *
 * Locks needed in:
 * Locks held temp:
 * Locks on return:
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
PVOID WINAPI RtlReAllocateHeap( HANDLE handle, ULONG flags, PVOID ptr, SIZE_T req_size )
{
    LLPERTHREAD *perthread;
    SIZE_T blk_size;
    LLHEAP *heap;
    DWORD  type;
    BOOL   valid;
    void  *pArena;
    void  *ret;

    if (LLHEAP_IsOrigHeap(handle))
        return RtlReAllocateHeapOrig(handle, flags, ptr, req_size);

    if (!ptr) return NULL;
    if (!(heap = LLHEAP_GetPtr( handle )))
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_HANDLE );
        return NULL;
    }

    /* Validate the parameters */

    flags &= HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY |
             HEAP_REALLOC_IN_PLACE_ONLY;
    flags |= heap->flags;

    blk_size = calc_blk_size_normal(flags, req_size);
    /* check for overflow */
    if (blk_size < req_size)
    {
        if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus( STATUS_NO_MEMORY );
        return NULL;
    }

    if ((perthread = LLHEAP_GetPerThread(heap)))
    {
        if (have_other_thread_blocks(&perthread->fastother))
            LLHEAP_PurgeOtherFastBlocks(perthread);
    }

    ret    = NULL;
    valid  = TRUE;
    pArena = user_ptr_to_arena(ptr);
    type   = LLHEAP_BlockType(pArena);
    if (type == LLARENA_FLAG_FAST)
    {
        /* fast blocks can be smaller than all other blocks */
        blk_size = max(LLHEAP_MIN_FAST_BLOCK_SIZE, blk_size);
        if (flags & HEAP_VALIDATE)
            valid = LLHEAP_ValidateFastBlock(heap, pArena, FALSE);
        if (valid && (perthread || (perthread = LLHEAP_AllocatePerThread(heap))))
            ret = LLHEAP_ReAllocateFastBlock(perthread, flags, pArena, req_size, blk_size);
        if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );
        return ret;
    }

    blk_size = max(LLHEAP_MIN_BLOCK_SIZE, blk_size);
    if (type == LLARENA_FLAG_THREAD)
    {
        /* handle per-thread blocks outside of lock */
        if (flags & HEAP_VALIDATE)
            valid = LLHEAP_ValidateThreadBlock(heap, pArena, FALSE);
        if (valid && (perthread || (perthread = LLHEAP_AllocatePerThread(heap))))
            ret = LLHEAP_ReAllocateThreadBlock(perthread, flags, pArena, req_size, blk_size);
        if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );
        return ret;
    }

    RtlEnterCriticalSection(&heap->cs);
    LLHEAP_PurgeBuffers(heap, &heap->free_bufs);
    LLHEAP_PurgeBuffers(heap, &heap->decommit_bufs);

    if (flags & HEAP_VALIDATE)
    {
        switch (type)
        {
            case LLARENA_FLAG_NORMAL:
                valid = LLHEAP_ValidateNormalBlock(heap, pArena, FALSE);
                break;
            case LLARENA_FLAG_LARGE:
                valid = LLHEAP_ValidateLargeBlock(heap, ptr);
                break;
            default:
                valid = FALSE;
                break;
        }
    }

    if (valid)
    {
        switch (type)
        {
            case LLARENA_FLAG_NORMAL:
                ret = LLHEAP_ReAllocateNormalBlock(heap, flags, pArena, req_size, blk_size);
                break;
            case LLARENA_FLAG_LARGE:
                ret = LLHEAP_ReAllocateLargeBlock(heap, flags, ptr, req_size); /* req_size on purpose */
                break;
            default:
                break;
        }
    }

    RtlLeaveCriticalSection(&heap->cs);
    if (!ret && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( STATUS_NO_MEMORY );

    return ret;
}


/***********************************************************************
 *           RtlCompactLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
 *
 * Compact the free space in a Heap.
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *
 * RETURNS
 *  Largest committed free block in bytes.  Mainly used for debugging
 *  For LLHEAP, it's used to test coalescing of blocks
 *
 */
ULONG WINAPI RtlCompactHeap( HANDLE heap, ULONG flags )
{
    static BOOL reported;

    if (LLHEAP_IsOrigHeap(heap))
        return RtlCompactHeapOrig(heap, flags);

    if (!reported++) FIXME( "(%p, 0x%x) stub\n", heap, flags );
    return 0;
}


/***********************************************************************
 *           RtlLockLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
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
    LLHEAP *heapPtr;

    if (LLHEAP_IsOrigHeap(heap))
        return RtlLockHeapOrig(heap);

    heapPtr = LLHEAP_GetPtr( heap );

    /* lock heap critical section */
    if (!heapPtr) return FALSE;
    RtlEnterCriticalSection(&heapPtr->cs);
    return TRUE;
}


/***********************************************************************
 *           RtlUnlockLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
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
    LLHEAP *heapPtr;

    if (LLHEAP_IsOrigHeap(heap))
        return RtlUnlockHeapOrig(heap);

    heapPtr = LLHEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    RtlLeaveCriticalSection(&heapPtr->cs);
    return TRUE;
}


/***********************************************************************
 *           RtlSizeLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
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
SIZE_T WINAPI RtlSizeHeap( HANDLE handle, ULONG flags, const void *ptr )
{
    const void *pArena;
    LLHEAP *heap;
    DWORD type;
    SIZE_T ret;

    if (LLHEAP_IsOrigHeap(handle))
        return RtlSizeHeapOrig(handle, flags, ptr);

    if (!(heap = LLHEAP_GetPtr( handle )))
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_HANDLE );
        return ~0UL;
    }

    flags |= heap->flags;

    ret = ~0UL;
    pArena = user_ptr_to_arena(ptr);
    type   = LLHEAP_BlockType(pArena);
    switch (type)
    {
        case LLARENA_FLAG_FAST:
        {
            const LLARENA_FAST_BLOCK *block = (const LLARENA_FAST_BLOCK *)pArena;
            LLGROUP *group = get_cluster(block)->group;
            return blk_to_rnd_size(group->blk_size) - block->unused;
        }

        case LLARENA_FLAG_THREAD:
        {
            const LLARENA_THREAD_BLOCK *block = (const LLARENA_THREAD_BLOCK *)pArena;
            return blk_to_rnd_size(get_blk_size_thread(block)) - block->unused;
        }
    }

    RtlEnterCriticalSection(&heap->cs);
    switch (type)
    {
        case LLARENA_FLAG_NORMAL:
            {
                const LLARENA_NORMAL_BLOCK *pNormal = (const LLARENA_NORMAL_BLOCK *)pArena;
                ret = get_rnd_size_normal(pNormal) - pNormal->unused;
            }
            break;
        case LLARENA_FLAG_LARGE:
            {
                /* LLARENA_LARGE has a different size from normal blocks */
                const LLARENA_LARGE *large_arena = (const LLARENA_LARGE *)ptr - 1;
                ret = large_arena->act_size;
            }
            break;
        default:
            ERR("%u:%lu: Heap %p: Invalid block type on %p\n",
                getpid(), pthread_self(), heap, ptr);
            break;
    }

    RtlLeaveCriticalSection(&heap->cs);
    return ret;
}


/***********************************************************************
 *           RtlValidateLLHeap   (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
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
    LLHEAP *heapPtr;

    if (LLHEAP_IsOrigHeap(heap))
        return RtlValidateHeapOrig(heap, flags, ptr);

    heapPtr = LLHEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    return LLHEAP_IsRealArena( heapPtr, flags, ptr, QUIET );
}


/***********************************************************************
 *           RtlWalkHeap    (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
 *
 * FIXME
 *  The PROCESS_HEAP_ENTRY flag values seem different between this
 *  function and HeapWalk(). To be checked.
 */
NTSTATUS WINAPI RtlWalkHeap( HANDLE heap, PVOID entry_ptr )
{
    if (LLHEAP_IsOrigHeap(heap))
        return RtlWalkHeapOrig(heap, entry_ptr);

    FIXME("%u:%lu: stub\n", getpid(), pthread_self());
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *           RtlGetProcessLLHeaps    (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
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
    ULONG total;
    struct list *ptr;

    total = RtlGetProcessHeapsOrig( count, heaps );
    if (!processLLHeap || (total >= count))
        return total;

    heaps += total;
    ++total;        /* include main heap */
    RtlEnterCriticalSection( &processLLHeap->cs );
    LIST_FOR_EACH( ptr, &processLLHeap->entry ) total++;
    if (total <= count)
    {
        *heaps++ = processLLHeap;
        LIST_FOR_EACH( ptr, &processLLHeap->entry )
            *heaps++ = LIST_ENTRY( ptr, LLHEAP, entry );
    }
    RtlLeaveCriticalSection( &processLLHeap->cs );
    return total;
}


/***********************************************************************
 *           RtlQueryLLHeapInformation    (NTDLL.@)
 *
 * Locks needed in: none
 * Locks held temp: none
 * Locks on return: none
 */
NTSTATUS WINAPI RtlQueryHeapInformation( HANDLE heap, HEAP_INFORMATION_CLASS info_class,
                                           PVOID info, SIZE_T size_in, PSIZE_T size_out)
{
    if (!heap)
        return STATUS_INVALID_PARAMETER;

    if (!info)
        return STATUS_ACCESS_VIOLATION;

    if (LLHEAP_IsOrigHeap(heap))
        return RtlQueryHeapInformationOrig(heap, info_class, info, size_in, size_out);

    switch (info_class)
    {
    case HeapCompatibilityInformation:
        if (size_out) *size_out = sizeof(ULONG);

        if (size_in < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *(ULONG *)info = 2; /* llheap */
        return STATUS_SUCCESS;

    default:
        FIXME("Unknown heap information class %u\n", info_class);
        return STATUS_INVALID_INFO_CLASS;
    }
}
