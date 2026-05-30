/*
 * aalloc.c
 *
 * Heap allocator: segregated free lists + boundary-tag coalescing.
 *
 * Every live block on the heap has a two-word header (info | span).
 * info packs the block size into the upper bits and a 2-bit tag into
 * the lower bits.  span records the size of the preceding block so
 * the left neighbour can be found in O(1) without an extra pointer.
 *
 * Free blocks carry forward/backward pointers that occupy the same
 * eight bytes the caller would use for data — those two fields share
 * a union.
 *
 * Bins are indexed by usable size / 8.  Bin 0 holds 8-byte blocks,
 * bin 58 is a catch-all for anything larger.
 *
 * Milestones that land here later:
 *   - Thread-local arenas (see LOCK / UNLOCK and the TODO below)
 *   - Slab allocator for small objects (see slab section)
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

/* =========================================================================
 * Block layout
 * =========================================================================
 *
 *  [ info ][ span ][ fd | bk  (free) / body (alloc) ]
 *
 *  info  bits 63..2  block size (8-byte aligned, so bits 1:0 are free)
 *        bits  1..0  tag: TAG_FREE | TAG_ALLOC | TAG_GUARD
 *
 *  span  size of the block to the left in memory; lets us walk backwards
 *        in O(1) for coalescing without a second pointer
 */

typedef struct blk {
    size_t info;
    size_t span;
    union {
        struct { struct blk *fd; struct blk *bk; };
        char body[0];
    };
} blk_t;

/* Block tags stored in info bits 1:0 */
#define TAG_FREE   0
#define TAG_ALLOC  1
#define TAG_GUARD  2

/* Bytes consumed by info + span; fd/bk overlap with user data */
#define OVERHEAD   (sizeof(blk_t) - 2 * sizeof(blk_t *))

/* Accessors — size and tag packed into one word */
#define SZ(b)          ((b)->info & ~(size_t)3)
#define TAG(b)         ((int)((b)->info & 3))
#define SET_SZ(b,s)    ((b)->info = (s)  | ((b)->info & 3))
#define SET_TAG(b,t)   ((b)->info = ((b)->info & ~(size_t)3) | (size_t)(t))

/* Walk to adjacent blocks */
#define NEXT(b)        ((blk_t *)((char *)(b) + SZ(b)))
#define PREV(b)        ((blk_t *)((char *)(b) - (b)->span))

/* Strip OVERHEAD from a user pointer to reach the block header */
#define HDR(p)         ((blk_t *)((char *)(p) - OVERHEAD))

/* =========================================================================
 * Allocator constants
 * ========================================================================= */

#ifndef HEAP_CHUNK
#  define HEAP_CHUNK  4096   /* bytes requested per sbrk call       */
#endif

#define NUM_BINS   59        /* segregated bins; bin 58 = catch-all */
#define MAX_SEGS   1024      /* OS segments we track for audit      */

/* =========================================================================
 * Global heap state
 *
 * All mutable allocator state lives in one struct so it is easy to swap
 * in a per-arena version for thread-local allocation (Milestone 2).
 * ========================================================================= */

static struct heap {
    blk_t  bins[NUM_BINS]; /* free-list sentinels, one ring per bin */
    blk_t *frontier;       /* right guard of the most recent segment */
    void  *origin;         /* base address for offset printing      */
    blk_t *segs[MAX_SEGS]; /* left guard of every OS segment        */
    size_t nseg;
} H;

static bool heap_ready = false;

/* -------------------------------------------------------------------------
 * TODO Milestone 2 — thread-local arenas
 *
 * Replace the global lock below with per-arena mutexes.
 * Each thread should acquire its own heap_t from a pool,
 * eliminating contention on the malloc fast path.
 * ------------------------------------------------------------------------- */
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()    pthread_mutex_lock(&global_lock)
#define UNLOCK()  pthread_mutex_unlock(&global_lock)

/* -------------------------------------------------------------------------
 * TODO Milestone 3 — slab allocator
 *
 * For requests below a threshold (e.g. 256 bytes), bypass the segregated
 * lists and serve from a per-size-class slab.  Hook in here:
 *
 *   if (size <= SLAB_MAX) return slab_alloc(size);
 * ------------------------------------------------------------------------- */

/* =========================================================================
 * Heap growth
 *
 * Maps one HEAP_CHUNK from the OS, installs guard blocks at both ends,
 * and returns the single usable block in between.
 * ========================================================================= */

/*
 * grow_heap — map one HEAP_CHUNK from the OS, install guards at both ends,
 * and return the interior block.  Does NOT insert into any free list or
 * segment tracker — the caller decides both, since adjacency determines
 * whether the new segment is independent or merged with the previous one.
 */
static blk_t *grow_heap(void) {
    void *raw = sbrk((intptr_t)HEAP_CHUNK);
    if (raw == (void *)-1) return NULL;

    blk_t *lg  = (blk_t *)raw;
    lg->info   = (size_t)TAG_GUARD | OVERHEAD;
    lg->span   = OVERHEAD;

    blk_t *blk = (blk_t *)((char *)raw + OVERHEAD);
    blk->info  = (HEAP_CHUNK - 2 * OVERHEAD) | TAG_FREE;
    blk->span  = OVERHEAD;

    blk_t *rg  = (blk_t *)((char *)raw + HEAP_CHUNK - OVERHEAD);
    rg->info   = (size_t)TAG_GUARD | OVERHEAD;
    rg->span   = HEAP_CHUNK - 2 * OVERHEAD;

    return blk;
}

/* =========================================================================
 * User-written allocator logic
 *
 * The six functions below are the core of the allocator.  The surrounding
 * infrastructure (block layout, heap growth, bin indexing, verification)
 * is written fresh; these six are the work done in the implementation.
 * ========================================================================= */

/*
 * bin_for — map usable byte count to a segregated bin index.
 *
 * All block sizes are multiples of 8, so dividing by 8 gives a dense
 * index starting at 0.  Anything past bin 57 lands in the catch-all.
 */
static size_t bin_for(size_t usable) {
    size_t idx = usable / 8 - 1;
    if (idx > NUM_BINS - 1)
        idx = NUM_BINS - 1;
    return idx;
}

/*
 * list_push — prepend blk into the free list rooted at sentinel.
 */
static void list_push(blk_t *sentinel, blk_t *blk) {
    blk->fd = sentinel->fd;
    blk->bk = sentinel;
    sentinel->fd->bk = blk;
    sentinel->fd = blk;
}

/*
 * list_drop — unlink blk from whatever list it currently sits in.
 */
static void list_drop(blk_t *blk) {
    blk->bk->fd = blk->fd;
    blk->fd->bk = blk->bk;
}

/*
 * find_fit — search the segregated bins for a block of at least total_size
 * bytes, growing the heap as needed.
 *
 * Starting at naive_bin, walk upward until a non-empty bin is found.
 * Fixed-size bins: take the head directly.
 * Catch-all bin (58): scan for a block large enough.
 *
 * When every bin is empty, call grow_heap.  If the new segment is
 * adjacent to the previous one, reclaim the two guard blocks as usable
 * space — either by absorbing them into an existing free block at the
 * segment boundary, or by bridging the gap directly.
 */
static blk_t *find_fit(size_t naive_bin, size_t total_size) {
    size_t idx = naive_bin;
    while (idx < NUM_BINS) {
        blk_t *sentinel = &H.bins[idx];
        if (sentinel->fd != sentinel)
            break;
        idx++;
    }

    blk_t *found = NULL;

    if (idx < NUM_BINS) {
        blk_t *sentinel = &H.bins[idx];
        if (idx < NUM_BINS - 1) {
            found = sentinel->fd;
        } else {
            blk_t *cur = sentinel->fd;
            while (cur != sentinel && SZ(cur) < total_size)
                cur = cur->fd;
            if (cur != sentinel)
                found = cur;
        }
    }

    while (found == NULL) {
        blk_t *saved_frontier = H.frontier;
        blk_t *fresh          = grow_heap();
        blk_t *new_frontier   = NEXT(fresh);

        if ((char *)fresh - OVERHEAD == (char *)saved_frontier + OVERHEAD) {
            /*
             * Adjacent segment: absorb the two guard blocks between the
             * chunks as usable heap space, eliminating 2*OVERHEAD of waste.
             *
             * Two sub-cases depending on whether the block just before the
             * old right guard is free (coalesce) or allocated (bridge).
             *
             * In either case the new segment's left guard is NOT registered
             * in H.segs — it is interior to the combined segment, so the
             * auditor must not try to walk from it independently.
             */
            blk_t *prev = PREV(saved_frontier);

            if (TAG(prev) == TAG_FREE) {
                /* Grow prev across both guards and the fresh interior. */
                list_drop(prev);
                size_t merged = SZ(prev) + 2 * OVERHEAD + SZ(fresh);
                SET_SZ(prev, merged);
                new_frontier->span = merged;
                list_push(&H.bins[bin_for(merged - OVERHEAD)], prev);
            } else {
                /*
                 * No adjacent free block; reclaim the two guards as a new
                 * free block starting at saved_frontier.
                 */
                blk_t *bridge     = saved_frontier;
                size_t bridged_sz = 2 * OVERHEAD + SZ(fresh);
                SET_SZ(bridge, bridged_sz);
                SET_TAG(bridge, TAG_FREE);
                new_frontier->span = bridged_sz;   /* keep boundary tag consistent */
                list_push(&H.bins[bin_for(bridged_sz - OVERHEAD)], bridge);
            }
        } else {
            /* Non-adjacent: register as an independent segment. */
            blk_t *lg = (blk_t *)((char *)fresh - OVERHEAD);
            if (H.nseg < MAX_SEGS)
                H.segs[H.nseg++] = lg;
            list_push(&H.bins[bin_for(SZ(fresh) - OVERHEAD)], fresh);
        }

        H.frontier = new_frontier;
        found = find_fit(naive_bin, total_size);
    }

    return found;
}

/*
 * cut_block — remove candidate from its bin, split off the requested
 * total_size from the high end, and return the remainder to the
 * appropriate bin (skipping the remove+reinsert when the bin stays the same).
 */
static blk_t *cut_block(blk_t *candidate, size_t total_size) {
    size_t blk_sz  = SZ(candidate);
    size_t rem_sz  = blk_sz - total_size;

    size_t old_idx = bin_for(blk_sz  - OVERHEAD);
    size_t rem_idx = bin_for(rem_sz  - OVERHEAD);

    if (blk_sz >= total_size && rem_sz < sizeof(blk_t)) {
        list_drop(candidate);
        SET_TAG(candidate, TAG_ALLOC);
        return candidate;
    }

    if (old_idx != rem_idx) {
        list_drop(candidate);
        SET_SZ(candidate, rem_sz);
        list_push(&H.bins[rem_idx], candidate);
    } else {
        SET_SZ(candidate, rem_sz);
    }

    blk_t *alloc = (blk_t *)((char *)candidate + rem_sz);
    SET_SZ(alloc, total_size);
    SET_TAG(alloc, TAG_ALLOC);
    alloc->span = rem_sz;

    NEXT(alloc)->span = total_size;

    return alloc;
}

/*
 * merge_free — coalesce blk with its immediate neighbours and insert the
 * merged result into the correct bin.
 *
 * Four cases based on left/right neighbour state:
 *   1. both allocated  — insert blk as-is
 *   2. right free      — absorb right into blk
 *   3. left free       — absorb blk into left
 *   4. both free       — absorb all three
 *
 * When a coalesced block stays in the same bin (old_idx == new_idx),
 * we splice the new free block into the existing list node in-place
 * rather than doing a remove + re-insert.
 */
static void merge_free(blk_t *blk, blk_t *left, blk_t *right) {
    int   left_tag = TAG(left);
    int   right_tag = TAG(right);
    size_t curr_sz  = SZ(blk);

    blk_t *result      = blk;
    bool   skip_insert = false;

    if (left_tag != TAG_FREE && right_tag != TAG_FREE) {
        /* Case 1 */
        result = blk;

    } else if (left_tag != TAG_FREE && right_tag == TAG_FREE) {
        /* Case 2 — absorb right */
        size_t new_sz  = curr_sz + SZ(right);
        size_t old_idx = bin_for(SZ(right) - OVERHEAD);
        size_t new_idx = bin_for(new_sz    - OVERHEAD);

        if (old_idx != new_idx) {
            list_drop(right);
        } else {
            blk->fd         = right->fd;
            blk->bk         = right->bk;
            blk->fd->bk     = blk;
            blk->bk->fd     = blk;
            skip_insert = true;
        }
        SET_SZ(blk, new_sz);
        result = blk;

    } else if (left_tag == TAG_FREE && right_tag != TAG_FREE) {
        /* Case 3 — absorb into left */
        size_t new_sz  = SZ(left) + curr_sz;
        size_t old_idx = bin_for(SZ(left) - OVERHEAD);
        size_t new_idx = bin_for(new_sz   - OVERHEAD);

        if (old_idx != new_idx)
            list_drop(left);
        else
            skip_insert = true;

        SET_SZ(left, new_sz);
        result = left;

    } else {
        /* Case 4 — absorb left and right */
        list_drop(right);

        size_t new_sz  = SZ(left) + curr_sz + SZ(right);
        size_t old_idx = bin_for(SZ(left) - OVERHEAD);
        size_t new_idx = bin_for(new_sz   - OVERHEAD);

        if (old_idx != new_idx)
            list_drop(left);
        else
            skip_insert = true;

        SET_SZ(left, new_sz);
        result = left;
    }

    SET_TAG(result, TAG_FREE);
    NEXT(result)->span = SZ(result);

    if (!skip_insert) {
        size_t final_idx = bin_for(SZ(result) - OVERHEAD);
        list_push(&H.bins[final_idx], result);
    }
}

/* =========================================================================
 * Heap-level alloc / free
 * ========================================================================= */

static void *heap_alloc(size_t raw_size) {
    if (raw_size == 0)
        return NULL;

    size_t aligned = (raw_size + 7) & ~(size_t)7;
    size_t total   = aligned + OVERHEAD;
    if (total < sizeof(blk_t))
        total = sizeof(blk_t);

    size_t usable    = total - OVERHEAD;
    size_t naive_bin = bin_for(usable);

    blk_t *fit  = find_fit(naive_bin, total);
    blk_t *blk  = cut_block(fit, total);

    return blk->body;
}

static void heap_free(void *p) {
    if (p == NULL)
        return;

    blk_t *blk = HDR(p);

    if (TAG(blk) != TAG_ALLOC) {
        fprintf(stderr, "aalloc: double-free or corrupt block at %p\n", p);
        abort();
    }

    SET_TAG(blk, TAG_FREE);
    merge_free(blk, PREV(blk), NEXT(blk));
}

/* =========================================================================
 * Heap audit
 * ========================================================================= */

/*
 * Check every bin for cycles (Floyd) and broken next/prev links.
 */
static bool audit_bins(void) {
    for (int i = 0; i < NUM_BINS; i++) {
        blk_t *s = &H.bins[i];

        /* Cycle detection */
        blk_t *slow = s->fd, *fast = s->fd->fd;
        while (fast != s) {
            if (slow == fast) {
                fprintf(stderr, "aalloc: cycle in bin %d\n", i);
                return false;
            }
            slow = slow->fd;
            fast = fast->fd->fd;
        }

        /* Link consistency */
        for (blk_t *cur = s->fd; cur != s; cur = cur->fd) {
            if (cur->fd->bk != cur || cur->bk->fd != cur) {
                fprintf(stderr, "aalloc: broken link in bin %d\n", i);
                return false;
            }
        }
    }
    return true;
}

/*
 * Walk one segment (from its left guard to its right guard) verifying
 * that each block's size agrees with the following block's span.
 */
static bool audit_segment(blk_t *lg) {
    for (blk_t *cur = NEXT(lg); TAG(cur) != TAG_GUARD; cur = NEXT(cur)) {
        if (SZ(cur) != NEXT(cur)->span) {
            fprintf(stderr, "aalloc: boundary-tag mismatch\n");
            return false;
        }
    }
    return true;
}

static bool audit_heap(void) {
    for (size_t i = 0; i < H.nseg; i++)
        if (!audit_segment(H.segs[i]))
            return false;
    return audit_bins();
}

/* =========================================================================
 * Initialiser
 * ========================================================================= */

static void __attribute__((constructor)) heap_init(void) {
    if (heap_ready) return;

    for (int i = 0; i < NUM_BINS; i++) {
        H.bins[i].fd = &H.bins[i];
        H.bins[i].bk = &H.bins[i];
    }

    blk_t *first  = grow_heap();
    H.frontier    = NEXT(first);
    H.origin      = (char *)first - OVERHEAD;

    /* Register the first (and so far only) segment for the heap audit. */
    H.segs[H.nseg++] = (blk_t *)((char *)first - OVERHEAD);

    list_push(&H.bins[NUM_BINS - 1], first);

    heap_ready = true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void *aa_malloc(size_t size) {
    LOCK();
    void *p = heap_alloc(size);
    UNLOCK();
    return p;
}

void aa_free(void *p) {
    LOCK();
    heap_free(p);
    UNLOCK();
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
    return audit_heap();
}
