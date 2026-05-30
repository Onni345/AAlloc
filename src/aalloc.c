/*
 * aalloc.c — Core allocator: boundary-tag heap with segregated free lists.
 *
 * Design:
 *   - sbrk-based heap, carved into fixed-size chunks with fencepost guards
 *   - boundary tags: each block carries size+state in one packed word and
 *     a left_size field for O(1) left-neighbour lookup
 *   - 59 segregated bins (size-indexed); last bin is a catch-all for large blocks
 *   - four-case coalescing on every free
 *   - coarse pthread mutex; per-thread arenas added in a later milestone
 */

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include "aalloc.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

#ifndef AA_CHUNK_SIZE
#  define AA_CHUNK_SIZE 4096
#endif

#ifndef AA_NUM_BINS
#  define AA_NUM_BINS 59
#endif

#define AA_MAX_CHUNKS 1024

/* Overhead of the always-present fields (size_state + left_size).
 * The next/prev pointers live in the same space as user data — they are only
 * valid while the block is free. */
#define AA_HDR_SIZE  (sizeof(block_t) - 2 * sizeof(block_t *))

/* =========================================================================
 * Block state
 * ========================================================================= */

enum blk_state {
    BLK_FREE  = 0,   /* on a free list, available for allocation  */
    BLK_USED  = 1,   /* live allocation, owned by caller          */
    BLK_FENCE = 2,   /* sentinel guard at chunk boundaries        */
};

/* =========================================================================
 * Block metadata struct
 *
 * size_state packs the block size (aligned to 8, so low 3 bits are free)
 * with the two-bit state field in the least-significant bits.
 *
 * left_size holds the size of the immediately preceding block in memory,
 * enabling O(1) left-neighbour lookup without a separate reverse pointer.
 *
 * When BLK_FREE: next/prev thread the block into its free-list bin.
 * When BLK_USED: data[] is the first byte of caller-visible memory.
 * ========================================================================= */

typedef struct block {
    size_t size_state;
    size_t left_size;
    union {
        struct {
            struct block *next;
            struct block *prev;
        };
        char data[0];
    };
} block_t;

/* =========================================================================
 * Packed-field accessors
 *
 * Size is always a multiple of 8, so bits[1:0] are spare for the state.
 * ========================================================================= */

static inline size_t blk_size(block_t *b) {
    return b->size_state & ~(size_t)0x3;
}

static inline void blk_set_size(block_t *b, size_t sz) {
    b->size_state = sz | (b->size_state & 0x3);
}

static inline enum blk_state blk_state_of(block_t *b) {
    return (enum blk_state)(b->size_state & 0x3);
}

static inline void blk_set_state(block_t *b, enum blk_state s) {
    b->size_state = (b->size_state & ~(size_t)0x3) | (size_t)s;
}

/* =========================================================================
 * Pointer arithmetic helpers
 * ========================================================================= */

/* Return a block_t* that is `off` bytes from `ptr`. */
static inline block_t *blk_at(void *ptr, ptrdiff_t off) {
    return (block_t *)((char *)ptr + off);
}

/* The block immediately to the right in memory. */
static inline block_t *right_nbr(block_t *b) {
    return blk_at(b, (ptrdiff_t)blk_size(b));
}

/* The block immediately to the left, found via the boundary tag. */
static inline block_t *left_nbr(block_t *b) {
    return blk_at(b, -(ptrdiff_t)b->left_size);
}

/* Strip the AA_HDR_SIZE prefix from a user pointer to get the block. */
static inline block_t *user_to_blk(void *p) {
    return (block_t *)((char *)p - AA_HDR_SIZE);
}

/* =========================================================================
 * Global state
 * ========================================================================= */

static pthread_mutex_t mutex;

/* Sentinel nodes — one doubly-linked ring per bin. */
static block_t  bins[AA_NUM_BINS];

/* Points to the right fencepost of the most recently mapped chunk.
 * Used to detect adjacent OS allocations for chunk coalescing. */
static block_t *heap_end;

/* Base of the first chunk; used for deterministic pointer printing. */
static void    *heap_base;

/* Ordered list of left-fencepost pointers for boundary-tag verification. */
static block_t *chunk_list[AA_MAX_CHUNKS];
static size_t   chunk_count = 0;

static bool is_initialized = false;

static void init(void) __attribute__((constructor));

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static void     init(void);
static void    *allocate_object(size_t raw_size);
static void     deallocate_object(void *p);
static block_t *find_block_to_allocate(size_t bin_idx, size_t total_size);

/* =========================================================================
 * Low-level chunk/fencepost helpers
 * ========================================================================= */

static inline void make_fence(block_t *fp, size_t left_size) {
    blk_set_state(fp, BLK_FENCE);
    blk_set_size(fp, AA_HDR_SIZE);
    fp->left_size = left_size;
}

static inline void track_chunk(block_t *left_fp) {
    if (chunk_count < AA_MAX_CHUNKS)
        chunk_list[chunk_count++] = left_fp;
}

static inline void place_fences(void *raw, size_t size) {
    char *mem = (char *)raw;
    block_t *lf = (block_t *)mem;
    make_fence(lf, AA_HDR_SIZE);

    block_t *rf = blk_at(mem, (ptrdiff_t)(size - AA_HDR_SIZE));
    make_fence(rf, size - 2 * AA_HDR_SIZE);
}

/* =========================================================================
 * Bin index
 *
 * Maps an allocable (user-data) size to a bin.  The formula divides by 8
 * because all sizes are multiples of 8, giving a dense 1-based index.
 * Anything that would exceed the bin array lands in the catch-all last bin.
 * ========================================================================= */

static size_t bin_index(size_t allocable_size) {
    size_t idx = allocable_size / 8 - 1;
    if (idx > AA_NUM_BINS - 1)
        idx = AA_NUM_BINS - 1;
    return idx;
}

/* =========================================================================
 * Free-list operations
 * ========================================================================= */

/* Insert blk at the front of the list rooted at sentinel (LIFO). */
static void bin_insert(block_t *sentinel, block_t *blk) {
    blk->next = sentinel->next;
    blk->prev = sentinel;
    sentinel->next->prev = blk;
    sentinel->next = blk;
}

/* Unlink blk from wherever it currently sits. */
static void bin_remove(block_t *blk) {
    blk->prev->next = blk->next;
    blk->next->prev = blk->prev;
}

/* =========================================================================
 * OS chunk allocation
 *
 * Calls sbrk to map AA_CHUNK_SIZE bytes, installs fenceposts around the
 * usable interior, and returns the single free block inside the chunk.
 * ========================================================================= */

static block_t *request_chunk(size_t size) {
    void *mem = sbrk((intptr_t)size);

    place_fences(mem, size);
    block_t *blk = blk_at(mem, (ptrdiff_t)AA_HDR_SIZE);
    blk_set_state(blk, BLK_FREE);
    blk_set_size(blk, size - 2 * AA_HDR_SIZE);
    blk->left_size = AA_HDR_SIZE;
    return blk;
}

/* =========================================================================
 * Heap search with on-demand chunk extension
 *
 * Walks bins starting at naive_bin_idx.  If nothing fits, calls
 * request_chunk and attempts to coalesce the new chunk with the previous
 * one when they are adjacent in memory (separated only by two fenceposts).
 * Recurses/retries after each extension until a fit is found.
 * ========================================================================= */

static block_t *find_block_to_allocate(size_t naive_bin_idx, size_t total_size) {
    /* Scan bins for a non-empty list that can satisfy total_size. */
    size_t idx = naive_bin_idx;
    while (idx < AA_NUM_BINS) {
        block_t *sentinel = &bins[idx];
        if (sentinel->next != sentinel)
            break;
        idx++;
    }

    block_t *candidate = NULL;

    if (idx < AA_NUM_BINS) {
        block_t *sentinel = &bins[idx];
        if (idx < AA_NUM_BINS - 1) {
            /* Fixed-size bin: every block fits by construction. */
            candidate = sentinel->next;
        } else {
            /* Catch-all bin: must scan for a block large enough. */
            block_t *cur = sentinel->next;
            while (cur != sentinel && blk_size(cur) < total_size)
                cur = cur->next;
            if (cur != sentinel)
                candidate = cur;
        }
    }

    /* Extend the heap when no fitting block exists. */
    while (candidate == NULL) {
        block_t *saved_end = heap_end;
        block_t *fresh     = request_chunk(AA_CHUNK_SIZE);
        block_t *new_end   = right_nbr(fresh);

        /* Adjacent chunks: saved_end and the new chunk's left fencepost are
         * separated by exactly 2 * AA_HDR_SIZE.  Coalesce by absorbing the
         * two fenceposts into free space. */
        if ((char *)fresh - AA_HDR_SIZE == (char *)saved_end + AA_HDR_SIZE) {
            block_t *prev_blk = left_nbr(saved_end);

            if (blk_state_of(prev_blk) == BLK_FREE) {
                /* Merge prev_blk + two fenceposts + entire fresh chunk. */
                bin_remove(prev_blk);
                size_t merged = blk_size(prev_blk)
                              + 2 * AA_HDR_SIZE
                              + blk_size(fresh);
                blk_set_size(prev_blk, merged);
                new_end->left_size = merged;

                size_t new_idx = bin_index(merged - AA_HDR_SIZE);
                bin_insert(&bins[new_idx], prev_blk);
            } else {
                /* Cannot block-coalesce, but still reclaim the two fenceposts. */
                block_t *bridged = saved_end;
                size_t bridged_sz = 2 * AA_HDR_SIZE + blk_size(fresh);
                blk_set_size(bridged, bridged_sz);
                blk_set_state(bridged, BLK_FREE);
                bin_insert(&bins[bin_index(bridged_sz - AA_HDR_SIZE)], bridged);
            }
        } else {
            /* Non-adjacent: treat as an independent chunk. */
            block_t *left_fp = blk_at(fresh, -(ptrdiff_t)AA_HDR_SIZE);
            track_chunk(left_fp);
            bin_insert(&bins[bin_index(blk_size(fresh) - AA_HDR_SIZE)], fresh);
        }

        heap_end  = new_end;
        candidate = find_block_to_allocate(naive_bin_idx, total_size);
    }

    return candidate;
}

/* =========================================================================
 * Block carving
 *
 * Removes candidate from its bin and either uses it whole (when the
 * remainder would be too small to stand alone) or splits it, keeping the
 * remainder as a free block in the appropriate bin.
 *
 * The allocated portion is always taken from the high end of candidate so
 * that the remainder retains the original block's position in the list when
 * moving to the same bin, avoiding a remove + re-insert in that case.
 * ========================================================================= */

static block_t *carve_block(block_t *candidate, size_t total_size) {
    size_t blk_sz  = blk_size(candidate);
    size_t rem_sz  = blk_sz - total_size;

    size_t old_idx = bin_index(blk_sz     - AA_HDR_SIZE);
    size_t rem_idx = bin_index(rem_sz     - AA_HDR_SIZE);

    if (blk_sz >= total_size && rem_sz < sizeof(block_t)) {
        /* Use the whole block — remainder too small to hold free-list ptrs. */
        bin_remove(candidate);
        blk_set_state(candidate, BLK_USED);
        return candidate;
    }

    /* Split: remainder occupies the low end, allocation takes the high end. */
    if (old_idx != rem_idx) {
        bin_remove(candidate);
        blk_set_size(candidate, rem_sz);
        bin_insert(&bins[rem_idx], candidate);
    } else {
        /* Same bin: just shrink in-place, no list pointer update needed. */
        blk_set_size(candidate, rem_sz);
    }

    block_t *allocated = blk_at(candidate, (ptrdiff_t)rem_sz);
    blk_set_size(allocated, total_size);
    blk_set_state(allocated, BLK_USED);
    allocated->left_size = rem_sz;

    right_nbr(allocated)->left_size = total_size;

    return allocated;
}

/* =========================================================================
 * allocate_object — top-level allocator
 *
 * Rounds raw_size up to an 8-byte multiple, sizes the total block (data +
 * header overhead), finds a free block, carves it, and returns the user
 * data region past the header.
 * ========================================================================= */

static void *allocate_object(size_t raw_size) {
    if (raw_size == 0)
        return NULL;

    /* Round up to next multiple of 8. */
    size_t rounded = (raw_size + 7) & ~(size_t)7;

    /* Total block = rounded user data + always-present header fields. */
    size_t total = rounded + AA_HDR_SIZE;

    /* A free block must fit at least the full block_t so next/prev exist. */
    if (total < sizeof(block_t))
        total = sizeof(block_t);

    size_t allocable = total - AA_HDR_SIZE;
    size_t naive_idx = bin_index(allocable);

    block_t *fit       = find_block_to_allocate(naive_idx, total);
    block_t *allocated = carve_block(fit, total);

    /* Return pointer to data region (past the two always-present fields). */
    return (void *)((char *)allocated + AA_HDR_SIZE);
}

/* =========================================================================
 * Four-case coalescing
 *
 * Case 1: both neighbours allocated  — insert blk as-is
 * Case 2: right neighbour free        — merge right into blk
 * Case 3: left neighbour free         — merge blk into left
 * Case 4: both neighbours free        — merge all three together
 *
 * In cases 2-4, when the absorbing block stays in the same bin after the
 * merge (old_idx == new_idx) we splice the newly freed block into the
 * existing list node to avoid an unnecessary remove + re-insert.
 * ========================================================================= */

static void coalesce_and_free(block_t *blk, block_t *left, block_t *right) {
    enum blk_state left_state  = blk_state_of(left);
    enum blk_state right_state = blk_state_of(right);
    size_t curr_sz = blk_size(blk);

    block_t *to_insert     = blk;
    bool     skip_reinsert = false;

    if (left_state != BLK_FREE && right_state != BLK_FREE) {
        /* Case 1: isolated free block, nothing to merge. */
        to_insert = blk;

    } else if (left_state != BLK_FREE && right_state == BLK_FREE) {
        /* Case 2: absorb right neighbour. */
        size_t new_sz  = curr_sz + blk_size(right);
        size_t old_idx = bin_index(blk_size(right) - AA_HDR_SIZE);
        size_t new_idx = bin_index(new_sz - AA_HDR_SIZE);

        if (old_idx != new_idx) {
            bin_remove(right);
        } else {
            /* Splice blk into right's list position — no move needed. */
            blk->next           = right->next;
            blk->prev           = right->prev;
            blk->next->prev     = blk;
            blk->prev->next     = blk;
            skip_reinsert = true;
        }
        blk_set_size(blk, new_sz);
        to_insert = blk;

    } else if (left_state == BLK_FREE && right_state != BLK_FREE) {
        /* Case 3: merge blk into left. */
        size_t new_sz  = blk_size(left) + curr_sz;
        size_t old_idx = bin_index(blk_size(left) - AA_HDR_SIZE);
        size_t new_idx = bin_index(new_sz - AA_HDR_SIZE);

        if (old_idx != new_idx)
            bin_remove(left);
        else
            skip_reinsert = true;

        blk_set_size(left, new_sz);
        to_insert = left;

    } else {
        /* Case 4: both neighbours free — absorb both. */
        bin_remove(right); /* right always needs to leave its bin */

        size_t new_sz  = blk_size(left) + curr_sz + blk_size(right);
        size_t old_idx = bin_index(blk_size(left) - AA_HDR_SIZE);
        size_t new_idx = bin_index(new_sz - AA_HDR_SIZE);

        if (old_idx != new_idx)
            bin_remove(left);
        else
            skip_reinsert = true;

        blk_set_size(left, new_sz);
        to_insert = left;
    }

    /* Common post-merge bookkeeping. */
    blk_set_state(to_insert, BLK_FREE);
    right_nbr(to_insert)->left_size = blk_size(to_insert);

    if (!skip_reinsert) {
        size_t final_idx = bin_index(blk_size(to_insert) - AA_HDR_SIZE);
        bin_insert(&bins[final_idx], to_insert);
    }
}

/* =========================================================================
 * deallocate_object — top-level free
 * ========================================================================= */

static void deallocate_object(void *p) {
    if (p == NULL)
        return;

    block_t *blk = user_to_blk(p);

    if (blk_state_of(blk) != BLK_USED) {
        fprintf(stderr, "aalloc: double-free detected at %p\n", p);
        abort();
    }

    blk_set_state(blk, BLK_FREE);
    coalesce_and_free(blk, left_nbr(blk), right_nbr(blk));
}

/* =========================================================================
 * Verification
 * ========================================================================= */

/* Floyd's tortoise-and-hare cycle detection across all bins. */
static block_t *detect_bin_cycle(void) {
    for (int i = 0; i < AA_NUM_BINS; i++) {
        block_t *sentinel = &bins[i];
        block_t *slow = sentinel->next;
        block_t *fast = sentinel->next->next;
        for (; fast != sentinel; slow = slow->next, fast = fast->next->next)
            if (slow == fast)
                return slow;
    }
    return NULL;
}

/* Verify next/prev consistency across all bins. */
static block_t *detect_broken_links(void) {
    for (int i = 0; i < AA_NUM_BINS; i++) {
        block_t *sentinel = &bins[i];
        for (block_t *cur = sentinel->next; cur != sentinel; cur = cur->next)
            if (cur->next->prev != cur || cur->prev->next != cur)
                return cur;
    }
    return NULL;
}

static bool verify_freelist(void) {
    if (detect_bin_cycle()) {
        fprintf(stderr, "aalloc: cycle detected in free list\n");
        return false;
    }
    if (detect_broken_links()) {
        fprintf(stderr, "aalloc: broken next/prev link in free list\n");
        return false;
    }
    return true;
}

/* Walk one OS chunk (starting at its left fencepost) checking that
 * each block's size matches the next block's left_size field. */
static bool verify_chunk(block_t *chunk) {
    if (blk_state_of(chunk) != BLK_FENCE)
        return false;
    for (block_t *cur = right_nbr(chunk);
         blk_state_of(cur) != BLK_FENCE;
         cur = right_nbr(cur)) {
        if (blk_size(cur) != right_nbr(cur)->left_size)
            return false;
    }
    return true;
}

static bool verify_tags(void) {
    for (size_t i = 0; i < chunk_count; i++)
        if (!verify_chunk(chunk_list[i]))
            return false;
    return true;
}

/* =========================================================================
 * Initialiser
 * ========================================================================= */

static void init(void) {
    if (is_initialized)
        return;

    pthread_mutex_init(&mutex, NULL);

    /* Bootstrap sentinels — each bin is a circular list pointing to itself. */
    for (int i = 0; i < AA_NUM_BINS; i++) {
        bins[i].next = &bins[i];
        bins[i].prev = &bins[i];
    }

    /* Map the first chunk and remember its boundaries. */
    block_t *first = request_chunk(AA_CHUNK_SIZE);
    block_t *left_fp  = blk_at(first, -(ptrdiff_t)AA_HDR_SIZE);

    track_chunk(left_fp);
    heap_end  = right_nbr(first);
    heap_base = (char *)first - AA_HDR_SIZE;

    /* Place the first free block into the catch-all bin. */
    block_t *sentinel = &bins[AA_NUM_BINS - 1];
    sentinel->next = first;
    sentinel->prev = first;
    first->next    = sentinel;
    first->prev    = sentinel;

    is_initialized = true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void *aa_malloc(size_t size) {
    pthread_mutex_lock(&mutex);
    void *ptr = allocate_object(size);
    pthread_mutex_unlock(&mutex);
    return ptr;
}

void aa_free(void *p) {
    pthread_mutex_lock(&mutex);
    deallocate_object(p);
    pthread_mutex_unlock(&mutex);
}

void *aa_calloc(size_t nmemb, size_t size) {
    void *ptr = aa_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

void *aa_realloc(void *ptr, size_t size) {
    void *mem = aa_malloc(size);
    if (mem && ptr) memcpy(mem, ptr, size);
    aa_free(ptr);
    return mem;
}

bool aa_verify(void) {
    return verify_freelist() && verify_tags();
}
