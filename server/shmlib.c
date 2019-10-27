/*
 * Server-side shared memory management
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

#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdio.h>      // TODO:

extern const char *wine_get_config_dir(void);

/*
 shm_ptr_t is currently always 32-bit.  it is converted to 32/64-bit by client
 31                          0
 |segment|  offset           |
*/
#define MAX_SHM_SEGS_BITS   6
#define MAX_SHM_SEGS_SHIFT  (sizeof(shm_ptr_t) * 8 - MAX_SHM_SEGS_BITS)
#define MAX_SHM_SEGS        (1<<MAX_SHM_SEGS_BITS)
#define MAX_SHM_SEGS_MASK   ((long)(MAX_SHM_SEGS-1) << MAX_SHM_SEGS_SHIFT)

#define MAX_SHM_OFFSET_BITS MAX_SHM_SEGS_SHIFT
#define MAX_SHM_OFFSET      (1<<MAX_SHM_OFFSET_BITS)
#define MAX_SHM_OFFSET_MASK (MAX_SHM_OFFSET - 1)

#define MAX_SHM_SEG_SIZE    (1024 * 1024)
/* TODO: C_ASSERT(MAX_SHM_SEG_SIZE <= MAX_SHM_OFFSET);*/

#define ALIGNMENT           (2*sizeof(void*))
#define ALIGNMENT_OFFSET    (ALIGNMENT - sizeof(struct shm_header))
#define ROUND_ALIGN(size)   (((size) + ALIGNMENT - 1) & ~(ALIGNMENT - 1))
#define ROUND_SIZE(size)    (ROUND_ALIGN(size) + ALIGNMENT_OFFSET)

#define MIN_ALLOC           ROUND_ALIGN(sizeof(struct shm_block))

#define SHM_FLAG_FREE       0x01
#define SHM_FLAG_PREV_FREE  0x02

struct shm_header
{
    unsigned int flags :  2;
    unsigned int size  : 30;
};

struct shm_block
{
    unsigned int flags :  2;
    unsigned int size  : 30;

    /* start of data area */
    struct list entry;
};

#define SHM_SEG_HDR_SIZE    ROUND_SIZE(sizeof(struct shm_segment))

struct shm_segment
{
    int idx;
    struct list freelist;   /* all free blocks sorted by size */
};

static inline key_t shm_get_key(int proj_id) {
    return ftok(wine_get_config_dir(), proj_id);
}

static struct shm_segment *segments[MAX_SHM_SEGS];

static const struct shm_block *get_segment_limit(const struct shm_segment *segment)
{
    return (const struct shm_block *)((char *)segment + MAX_SHM_SEG_SIZE);
}

static inline struct shm_block *get_first_block(struct shm_segment *segment)
{
    return (struct shm_block *)((char *)segment + SHM_SEG_HDR_SIZE);
}

static inline struct shm_block *get_next_block(struct shm_segment *segment,
                                               struct shm_block *block)
{
    struct shm_block *next;

    next = (struct shm_block *)((char *)block + block->size);
    if (next >= get_segment_limit(segment))
        return NULL;
    return next;
}

static inline void *block_to_data_ptr(struct shm_block *block)
{
    return (char*)block + sizeof(struct shm_header);
}

static inline shm_ptr_t ptr_to_shm_ptr(struct shm_segment *segment, struct shm_block *block)
{
    ptrdiff_t offset = (ptrdiff_t)block_to_data_ptr(block) - (ptrdiff_t)segment;
    return (segment->idx << MAX_SHM_SEGS_SHIFT) | (offset & MAX_SHM_OFFSET_MASK);
}

static inline struct shm_segment *segment_from_ptr(shm_ptr_t shm_ptr)
{
    unsigned int shmidx;

    shmidx = shm_ptr >> MAX_SHM_SEGS_SHIFT;
    if (shmidx >= MAX_SHM_SEGS)
        return NULL;
    return segments[shmidx];
}

static inline struct shm_block *shm_ptr_to_block(struct shm_segment *segment, shm_ptr_t shm_ptr)
{
    return (struct shm_block *)((char *)segment +
            (shm_ptr & MAX_SHM_OFFSET_MASK) - sizeof(struct shm_header));
}

void *shm_ptr_to_void_ptr(shm_ptr_t shmptr)
{
    struct shm_segment *segment;
    struct shm_block *block;

    if (shmptr == SHM_NULL)
        return NULL;

    if (!(segment = segment_from_ptr(shmptr)))
        return NULL;

    block = shm_ptr_to_block(segment, shmptr);
    return block_to_data_ptr(block);
}

static inline struct shm_block *create_block(void *base, ptrdiff_t offset, unsigned int blk_sz)
{
    struct shm_block *block;

    block = (struct shm_block *)((char *)base + offset);
    block->flags = 0;
    block->size = blk_sz;
    return block;
}

static void add_to_free_list(struct shm_segment *segment, struct shm_block *block)
{
    struct shm_block *this;
    struct shm_block *next;
    struct list *cur;

    next = get_next_block(segment, block);
    if (next)
    {
        next->flags |= SHM_FLAG_PREV_FREE;
        ((struct shm_block **)next)[-1] = block;
    }

    block->flags |= SHM_FLAG_FREE;
    cur = list_head(&segment->freelist);
    while (cur)
    {
        this = LIST_ENTRY(cur, struct shm_block, entry);
        if (block->size < this->size ||
            (block->size == this->size && (block < this)))
        {
            list_add_before(&this->entry, &block->entry);
            return;
        }
        cur = list_next(&segment->freelist, cur);
    }
    list_add_tail(&segment->freelist, &block->entry);
}

static struct shm_segment *get_segment(int idx)
{
    struct shm_segment *segment;
    struct shm_block *block;
    key_t key;
    int id;

    key = shm_get_key(idx);
    id = shmget(key, MAX_SHM_SEG_SIZE, 0600 | IPC_CREAT | IPC_EXCL);
    if (id == -1)
        return NULL;

    segment = shmat(id, NULL, 0);
    if (!segment)
        return NULL;

    segment->idx = idx;
    list_init(&segment->freelist);

    block = get_first_block(segment);
    block->size = MAX_SHM_SEG_SIZE - SHM_SEG_HDR_SIZE;
    add_to_free_list(segment, block);

    return segment;
}

static void shm_test(void)
{
  shm_ptr_t ptr[10];
  size_t size;
  void *user;
  int i;

  printf("%s: TODO REMOVE ME\n", __FUNCTION__);
  for (i = 0; i < sizeof(ptr)/sizeof(ptr[0]); i++)
  {
      size = rand() % 4096;
      ptr[i] = shm_malloc(size);
      user = shm_ptr_to_void_ptr(ptr[i]);
      memset(user, (i+1), 16);
      printf("%s: [% 2d] %08x %04zx (%zu)\n", __FUNCTION__, i, ptr[i], size, size);
  }

  for (i = 0; i < sizeof(ptr)/sizeof(ptr[0]); i += rand() % 5)
  {
      // printf("%s: [% 2d] %08x (freeing)\n", __FUNCTION__, i, ptr[i]);
      // shm_free(ptr[i]);
  }
}

static int shm_init(void)
{
    static int initialized;
    struct shmid_ds ds;
    int i, id;
    key_t key;

    if (initialized)
        return 0;

    /* purge existing bad ones */
    for (i = 0; i < MAX_SHM_SEGS; i++ )
    {
        key = shm_get_key(i);
        if ((id = shmget(key, 0, 0600 | IPC_CREAT)) != -1)
            shmctl(id, IPC_RMID, &ds);
    }

    segments[0] = get_segment(0);
    if (!segments[0])
        return errno;

    initialized = 1;
    shm_test();
    return 0;
}

static struct shm_block *find_block_from_segment(struct shm_segment *segment, size_t blk_sz)
{
    struct shm_block *block;
    struct list *cur;

    cur = list_head(&segment->freelist);
    while (cur)
    {
        block = LIST_ENTRY(cur, struct shm_block, entry);
        if (blk_sz <= block->size)
        {
            block->flags &= ~SHM_FLAG_FREE;
            list_remove(&block->entry);
            return block;
        }

        cur = list_next(&segment->freelist, cur);
    }

    return NULL;
}

static void split_block(struct shm_segment *segment, struct shm_block *block, size_t blk_sz)
{
    struct shm_block *next;
    size_t rem_sz;

    rem_sz = block->size - blk_sz;
    if (rem_sz >= MIN_ALLOC)
    {
        block->size = blk_sz;
        next = get_next_block(segment, block);
        if (next)
        {
            next->flags = SHM_FLAG_FREE;
            next->size = rem_sz;
            add_to_free_list(segment, next);
        }
    }
    else /* remainder is too small - notify next block we're not free */
    {
        next = get_next_block(segment, block);
        if (next)
            next->flags &= ~SHM_FLAG_PREV_FREE;
    }
}

static struct shm_block *merge_block(struct shm_segment *segment, struct shm_block *block)
{
    struct shm_block *next;
    struct shm_block *prev;

    next = get_next_block(segment, block);
    if (next && (next->flags & SHM_FLAG_FREE))
    {
        list_remove(&next->entry);
        block->size += next->size;
    }

    if (block->flags & SHM_FLAG_PREV_FREE)
    {
        prev = ((struct shm_block **)block)[-1];
        list_remove(&prev->entry);
        prev->size += block->size;
        block = prev;
    }

    return block;
}

shm_ptr_t shm_malloc(size_t sz)
{
    struct shm_block *block;
    size_t blk_sz;
    int i;

    if (shm_init())
        return SHM_NULL;

    sz += sizeof(struct shm_header);
    blk_sz = ROUND_ALIGN(sz);
    if (blk_sz < MIN_ALLOC)
        blk_sz = MIN_ALLOC;

    /* for each segment, try to find exact match */
    for (i = 0; i < MAX_SHM_SEGS; i++ )
    {
        if (!segments[i])
        {
            if (!(segments[i] = get_segment(i)))
                return SHM_NULL;
        }

        block = find_block_from_segment(segments[i], blk_sz);
        if (block)
        {
            split_block(segments[i], block, blk_sz);
            return ptr_to_shm_ptr(segments[i], block);
        }
    }

    return SHM_NULL;
}

void shm_free(shm_ptr_t ptr)
{
    struct shm_segment *segment;
    struct shm_block *block;

    if (ptr == SHM_NULL)
        return;

    segment = segment_from_ptr(ptr);
    if (!segment)
        return;

    block = shm_ptr_to_block(segment, ptr);
    block = merge_block(segment, block);
    add_to_free_list(segment, block);
}

extern int debug_level;
extern int foreground;

static void shm_dump(void)
{
    int i;
    key_t key;
    struct shm_block *block;
    struct shm_segment *segment;

    printf("MAX_SHM_SEGS_BITS    %d\n", MAX_SHM_SEGS_BITS);
    printf("MAX_SHM_SEGS_SHIFT   %ld\n", MAX_SHM_SEGS_SHIFT);
    printf("MAX_SHM_SEGS         %d\n", MAX_SHM_SEGS);
    printf("MAX_SHM_SEGS_MASK    %lx\n", MAX_SHM_SEGS_MASK);
    printf("MAX_SHM_SEG_SIZE     %d (%x)\n", MAX_SHM_SEG_SIZE, MAX_SHM_SEG_SIZE);

    printf("MAX_SHM_OFFSET_BITS  %ld\n", MAX_SHM_OFFSET_BITS);
    printf("MAX_SHM_OFFSET       %x\n", MAX_SHM_OFFSET);
    printf("MAX_SHM_OFFSET_MASK  %x\n", MAX_SHM_OFFSET_MASK);

    printf("ALIGNMENT            %zu\n", ALIGNMENT);
    printf("ALIGNMENT_OFFSET     %zu\n", ALIGNMENT_OFFSET);

    for (i = 0; i < MAX_SHM_SEGS; i++ )
    {
        segment = segments[i];
        if (!segment) continue; /* TODO: skip or stop loop? */
        key = shm_get_key(i);

        printf("[%d] %08x [%p - %p)\n", i, key, segment, get_segment_limit(segment));

        /* dump the blocks */
        block = get_first_block(segment);
        do
        {
            unsigned char *bytes = block_to_data_ptr(block);
            printf("    %p %p %c %c %8u 0x%06x: ", block, bytes,
                block->flags & SHM_FLAG_FREE ? 'F' : ' ',
                block->flags & SHM_FLAG_PREV_FREE ? 'P' : ' ',
                block->size, block->size);
            block = get_next_block(segment, block);
            for (i = 0; i < 16; i++)
                printf("%02x ", bytes[i]);
            printf("\n");
        } while (block);
    }
}

int shm_term(void)
{
    struct shmid_ds ds;
    int i, id;

    if (debug_level && foreground)
        shm_dump();

    for (i = 0; i < MAX_SHM_SEGS; i++ )
    {
        if (!segments[i]) continue; /* TODO: skip or stop loop? */
        if ((id = shmget(shm_get_key(i), 0, 0600 | IPC_CREAT)) != -1)
            shmctl(id, IPC_RMID, &ds);
    }
    return ENOSYS;
}

int shm_attach(void)
{
    return ENOSYS;
}

