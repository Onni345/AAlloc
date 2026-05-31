/*
 * aalloc.c
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

#include "aalloc.h"

typedef struct header {
    size_t size_state;
    size_t left_size;
    struct arena *owner;
    union {
        struct { struct header *next; struct header *prev; };
        char data[0];
    };
} header;

enum state { UNALLOCATED = 0, ALLOCATED = 1, FENCEPOST = 2 };

#define ALLOC_HEADER_SIZE  (sizeof(header) - 2 * sizeof(header *))

#define get_size(h)          ((h)->size_state & ~(size_t)3)
#define set_size(h, s)       ((h)->size_state = (s) | ((h)->size_state & 3))
#define get_state(h)         ((enum state)((h)->size_state & 3))
#define set_state(h, s)      ((h)->size_state = ((h)->size_state & ~(size_t)3) | (size_t)(s))

#define get_right_header(h)  ((header *)((char *)(h) + get_size(h)))
#define get_left_header(h)   ((header *)((char *)(h) - (h)->left_size))
#define ptr_to_header(p)     ((header *)((char *)(p) - ALLOC_HEADER_SIZE))

#ifndef ARENA_SIZE
#  define ARENA_SIZE  4096
#endif

#ifndef N_LISTS
#  define N_LISTS  59
#endif

#define MAX_OS_CHUNKS  1024

#ifndef N_ARENAS
#  define N_ARENAS  10
#endif

typedef struct arena {
    header          freelistSentinels[N_LISTS];
    header         *lastFencePost;
    void           *base;
    header         *osChunkList[MAX_OS_CHUNKS];
    size_t          numOsChunks;
    pthread_mutex_t mutex;
    bool            initialized;
} arena_t;

static arena_t         arenas[N_ARENAS];
static size_t          next_arena       = 0;
static pthread_key_t   arena_key;
static pthread_mutex_t arena_assign_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  init_once         = PTHREAD_ONCE_INIT;

#define LOCK(a)    pthread_mutex_lock(&(a)->mutex)
#define UNLOCK(a)  pthread_mutex_unlock(&(a)->mutex)

/* ---- TODO Milestone 3: slab allocator hook
 *   if (raw_size <= SLAB_MAX) return slab_alloc(raw_size);
 * ---- */

static header *allocate_chunk(size_t size);

static inline void set_fencepost(header *fp, size_t left_size) {
    set_state(fp, FENCEPOST);
    set_size(fp, ALLOC_HEADER_SIZE);
    fp->left_size = left_size;
}

static inline void place_fenceposts(void *raw, size_t size) {
    char *mem = (char *)raw;
    set_fencepost((header *)mem, ALLOC_HEADER_SIZE);
    set_fencepost((header *)(mem + size - ALLOC_HEADER_SIZE), size - 2 * ALLOC_HEADER_SIZE);
}

static size_t find_freelist_idx(size_t allocable_size) {
    size_t freelist_idx = allocable_size / 8 - 1;
    if (freelist_idx > N_LISTS - 1)
        freelist_idx = N_LISTS - 1;
    return freelist_idx;
}

static void insert_into_freelist(header *sentinel, header *block_to_insert) {
    block_to_insert->next = sentinel->next;
    block_to_insert->prev = sentinel;
    sentinel->next->prev  = block_to_insert;
    sentinel->next        = block_to_insert;
}

static void remove_from_freelist(header *block) {
    block->prev->next = block->next;
    block->next->prev = block->prev;
}

static header *allocate_chunk(size_t size) {
    void *mem = sbrk((intptr_t)size);
    place_fenceposts(mem, size);
    header *hdr = (header *)((char *)mem + ALLOC_HEADER_SIZE);
    set_state(hdr, UNALLOCATED);
    set_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
    hdr->left_size = ALLOC_HEADER_SIZE;
    return hdr;
}

static header *find_block_to_allocate(arena_t *arena, size_t naive_freelist_idx, size_t total_block_size) {
    size_t freelist_idx = naive_freelist_idx;
    while (freelist_idx < N_LISTS) {
        header *freelist = &arena->freelistSentinels[freelist_idx];
        if (freelist->next != freelist)
            break;
        freelist_idx++;
    }

    header *block_to_allocate = NULL;

    if (freelist_idx < N_LISTS) {
        header *sentinel = &arena->freelistSentinels[freelist_idx];
        if (freelist_idx < N_LISTS - 1) {
            block_to_allocate = sentinel->next;
        } else {
            header *cur = sentinel->next;
            while (cur != sentinel && get_size(cur) < total_block_size)
                cur = cur->next;
            if (cur != sentinel)
                block_to_allocate = cur;
        }
    }

    while (block_to_allocate == NULL) {
        header *saved_last_fp = arena->lastFencePost;
        header *fresh_chunk   = allocate_chunk(ARENA_SIZE);
        header *new_right_fp  = get_right_header(fresh_chunk);

        if ((char *)fresh_chunk - ALLOC_HEADER_SIZE ==
            (char *)saved_last_fp + ALLOC_HEADER_SIZE) {

            header *prev = get_left_header(saved_last_fp);

            if (get_state(prev) == UNALLOCATED) {
                remove_from_freelist(prev);
                size_t new_size = get_size(prev)
                                + 2 * ALLOC_HEADER_SIZE
                                + get_size(fresh_chunk);
                set_size(prev, new_size);
                new_right_fp->left_size = new_size;
                insert_into_freelist(
                    &arena->freelistSentinels[find_freelist_idx(new_size - ALLOC_HEADER_SIZE)],
                    prev);
            } else {
                header *bridge   = saved_last_fp;
                size_t bridge_sz = 2 * ALLOC_HEADER_SIZE + get_size(fresh_chunk);
                set_size(bridge, bridge_sz);
                set_state(bridge, UNALLOCATED);
                new_right_fp->left_size = bridge_sz;
                insert_into_freelist(
                    &arena->freelistSentinels[find_freelist_idx(bridge_sz - ALLOC_HEADER_SIZE)],
                    bridge);
            }
        } else {
            header *left_fp = (header *)((char *)fresh_chunk - ALLOC_HEADER_SIZE);
            if (arena->numOsChunks < MAX_OS_CHUNKS)
                arena->osChunkList[arena->numOsChunks++] = left_fp;
            insert_into_freelist(
                &arena->freelistSentinels[find_freelist_idx(
                    get_size(fresh_chunk) - ALLOC_HEADER_SIZE)],
                fresh_chunk);
        }

        arena->lastFencePost = new_right_fp;
        block_to_allocate = find_block_to_allocate(arena, naive_freelist_idx, total_block_size);
    }

    return block_to_allocate;
}

static header *allocate_block(arena_t *arena, header *block_to_allocate, size_t total_request_size) {
    size_t block_size     = get_size(block_to_allocate);
    size_t remainder_size = block_size - total_request_size;

    size_t old_idx = find_freelist_idx(block_size     - ALLOC_HEADER_SIZE);
    size_t rem_idx = find_freelist_idx(remainder_size - ALLOC_HEADER_SIZE);

    if (block_size >= total_request_size && remainder_size < sizeof(header)) {
        remove_from_freelist(block_to_allocate);
        set_state(block_to_allocate, ALLOCATED);
        return block_to_allocate;
    }

    if (old_idx != rem_idx) {
        remove_from_freelist(block_to_allocate);
        set_size(block_to_allocate, remainder_size);
        insert_into_freelist(&arena->freelistSentinels[rem_idx], block_to_allocate);
    } else {
        set_size(block_to_allocate, remainder_size);
    }

    header *usable_block = get_right_header(block_to_allocate);
    set_size(usable_block, total_request_size);
    set_state(usable_block, ALLOCATED);
    usable_block->left_size = remainder_size;
    get_right_header(usable_block)->left_size = total_request_size;

    return usable_block;
}

static void free_block(arena_t *arena, header *free, header *left, header *right) {
    int    left_state  = get_state(left);
    int    right_state = get_state(right);
    size_t curr_size   = get_size(free);

    header *block_to_insert     = free;
    bool    handled_reinsertion = false;

    if (left_state != UNALLOCATED && right_state != UNALLOCATED) {
        block_to_insert = free;

    } else if (left_state != UNALLOCATED && right_state == UNALLOCATED) {
        size_t new_size = curr_size + get_size(right);
        size_t old_idx  = find_freelist_idx(get_size(right) - ALLOC_HEADER_SIZE);
        size_t new_idx  = find_freelist_idx(new_size        - ALLOC_HEADER_SIZE);

        if (old_idx != new_idx) {
            remove_from_freelist(right);
        } else {
            free->next       = right->next;
            free->prev       = right->prev;
            free->next->prev = free;
            free->prev->next = free;
            handled_reinsertion = true;
        }
        set_size(free, new_size);
        block_to_insert = free;

    } else if (left_state == UNALLOCATED && right_state != UNALLOCATED) {
        size_t new_size = get_size(left) + curr_size;
        size_t old_idx  = find_freelist_idx(get_size(left) - ALLOC_HEADER_SIZE);
        size_t new_idx  = find_freelist_idx(new_size       - ALLOC_HEADER_SIZE);

        if (old_idx != new_idx)
            remove_from_freelist(left);
        else
            handled_reinsertion = true;

        set_size(left, new_size);
        block_to_insert = left;

    } else {
        remove_from_freelist(right);

        size_t new_size = get_size(left) + curr_size + get_size(right);
        size_t old_idx  = find_freelist_idx(get_size(left) - ALLOC_HEADER_SIZE);
        size_t new_idx  = find_freelist_idx(new_size       - ALLOC_HEADER_SIZE);

        if (old_idx != new_idx)
            remove_from_freelist(left);
        else
            handled_reinsertion = true;

        set_size(left, new_size);
        block_to_insert = left;
    }

    set_state(block_to_insert, UNALLOCATED);
    get_right_header(block_to_insert)->left_size = get_size(block_to_insert);

    if (!handled_reinsertion) {
        size_t final_idx = find_freelist_idx(get_size(block_to_insert) - ALLOC_HEADER_SIZE);
        insert_into_freelist(&arena->freelistSentinels[final_idx], block_to_insert);
    }
}

static void *allocate_object(arena_t *arena, size_t raw_size) {
    if (raw_size == 0)
        return NULL;

    size_t rounded = (raw_size + 7) & ~(size_t)7;
    size_t total   = rounded + ALLOC_HEADER_SIZE;
    if (total < sizeof(header))
        total = sizeof(header);

    size_t allocable = total - ALLOC_HEADER_SIZE;
    size_t naive_idx = find_freelist_idx(allocable);

    header *fit       = find_block_to_allocate(arena, naive_idx, total);
    header *allocated = allocate_block(arena, fit, total);
    allocated->owner  = arena;

    return (void *)((char *)allocated + ALLOC_HEADER_SIZE);
}

static void deallocate_object(arena_t *arena, header *blk) {
    if (get_state(blk) != ALLOCATED) {
        fprintf(stderr, "aalloc: double-free detected at %p\n", (void *)blk);
        abort();
    }
    set_state(blk, UNALLOCATED);
    free_block(arena, blk, get_left_header(blk), get_right_header(blk));
}

static void init_arena(arena_t *arena) {
    pthread_mutex_init(&arena->mutex, NULL);

    for (int i = 0; i < N_LISTS; i++) {
        arena->freelistSentinels[i].next = &arena->freelistSentinels[i];
        arena->freelistSentinels[i].prev = &arena->freelistSentinels[i];
    }

    header *block   = allocate_chunk(ARENA_SIZE);
    header *left_fp = (header *)((char *)block - ALLOC_HEADER_SIZE);

    if (arena->numOsChunks < MAX_OS_CHUNKS)
        arena->osChunkList[arena->numOsChunks++] = left_fp;

    arena->lastFencePost = get_right_header(block);
    arena->base          = (char *)block - ALLOC_HEADER_SIZE;

    header *sentinel = &arena->freelistSentinels[N_LISTS - 1];
    sentinel->next = block;
    sentinel->prev = block;
    block->next    = sentinel;
    block->prev    = sentinel;

    arena->initialized = true;
}

static void global_init(void) {
    pthread_key_create(&arena_key, NULL);
}

static arena_t *get_thread_arena(void) {
    pthread_once(&init_once, global_init);

    arena_t *a = pthread_getspecific(arena_key);
    if (a != NULL)
        return a;

    pthread_mutex_lock(&arena_assign_lock);
    a = &arenas[next_arena++ % N_ARENAS];
    pthread_mutex_unlock(&arena_assign_lock);

    if (!a->initialized)
        init_arena(a);

    pthread_setspecific(arena_key, a);
    return a;
}

static bool verify_freelist(arena_t *arena) {
    for (int i = 0; i < N_LISTS; i++) {
        header *sentinel = &arena->freelistSentinels[i];
        header *slow = sentinel->next, *fast = sentinel->next->next;
        for (; fast != sentinel; slow = slow->next, fast = fast->next->next) {
            if (slow == fast) {
                fprintf(stderr, "aalloc: cycle in free list %d\n", i);
                return false;
            }
        }
        for (header *cur = sentinel->next; cur != sentinel; cur = cur->next) {
            if (cur->next->prev != cur || cur->prev->next != cur) {
                fprintf(stderr, "aalloc: broken link in free list %d\n", i);
                return false;
            }
        }
    }
    return true;
}

static bool verify_chunk(header *left_fp) {
    for (header *cur = get_right_header(left_fp);
         get_state(cur) != FENCEPOST;
         cur = get_right_header(cur)) {
        if (get_size(cur) != get_right_header(cur)->left_size) {
            fprintf(stderr, "aalloc: boundary-tag mismatch\n");
            return false;
        }
    }
    return true;
}

static bool verify_tags(arena_t *arena) {
    for (size_t i = 0; i < arena->numOsChunks; i++)
        if (!verify_chunk(arena->osChunkList[i]))
            return false;
    return true;
}

static void __attribute__((constructor)) _init_on_load(void) {
    pthread_once(&init_once, global_init);
}

void *aa_malloc(size_t size) {
    arena_t *arena = get_thread_arena();
    LOCK(arena);
    void *p = allocate_object(arena, size);
    UNLOCK(arena);
    return p;
}

void aa_free(void *p) {
    if (p == NULL) return;
    header  *blk   = ptr_to_header(p);
    arena_t *arena = blk->owner;
    LOCK(arena);
    deallocate_object(arena, blk);
    UNLOCK(arena);
}

void *aa_calloc(size_t nmemb, size_t size) {
    void *p = aa_malloc(nmemb * size);
    if (p) memset(p, 0, nmemb * size);
    return p;
}

void *aa_realloc(void *ptr, size_t size) {
    void *p = aa_malloc(size);
    if (p && ptr) memcpy(p, ptr, size);
    aa_free(ptr);
    return p;
}

bool aa_verify(void) {
    arena_t *arena = get_thread_arena();
    return verify_freelist(arena) && verify_tags(arena);
}
