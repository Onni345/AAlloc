/*
 * tests/test_basic.c — Smoke tests for the public allocator API.
 *
 * Tests are written to pass against stub returns (NULL) and become
 * meaningful once the implementation is filled in.
 */

#include <string.h>
#include "harness.h"
#include "../include/aalloc.h"

/* --- malloc ------------------------------------------------------------ */

static void test_malloc_zero_returns_null(void) {
    AA_EXPECT_NULL(aa_malloc(0));
}

static void test_malloc_small(void) {
    void *p = aa_malloc(8);
    AA_EXPECT_NOTNULL(p);
    aa_free(p);
}

static void test_malloc_large(void) {
    void *p = aa_malloc(4096);
    AA_EXPECT_NOTNULL(p);
    aa_free(p);
}

static void test_malloc_many(void) {
    void *ptrs[64];
    for (int i = 0; i < 64; i++) ptrs[i] = aa_malloc(16 * (i + 1));
    for (int i = 0; i < 64; i++) AA_EXPECT_NOTNULL(ptrs[i]);
    for (int i = 0; i < 64; i++) aa_free(ptrs[i]);
}

/* --- free -------------------------------------------------------------- */

static void test_free_null_is_noop(void) {
    aa_free(NULL); /* must not crash */
    AA_EXPECT(1);
}

/* --- calloc ------------------------------------------------------------ */

static void test_calloc_zeroed(void) {
    char *p = aa_calloc(32, 1);
    if (p) {
        int ok = 1;
        for (int i = 0; i < 32; i++) ok &= (p[i] == 0);
        AA_EXPECT(ok);
        aa_free(p);
    }
}

/* --- realloc ----------------------------------------------------------- */

static void test_realloc_preserves_data(void) {
    char *p = aa_malloc(16);
    if (!p) return;
    memset(p, 0xAB, 16);
    char *q = aa_realloc(p, 64);
    if (!q) return;
    AA_EXPECT((unsigned char)q[0] == 0xAB);
    aa_free(q);
}

/* --- verify ------------------------------------------------------------ */

static void test_verify_after_ops(void) {
    void *a = aa_malloc(32);
    void *b = aa_malloc(64);
    aa_free(a);
    void *c = aa_malloc(16);
    aa_free(b);
    aa_free(c);
    AA_EXPECT(aa_verify());
}

/* --- main -------------------------------------------------------------- */

int main(void) {
    AA_TEST(test_malloc_zero_returns_null);
    AA_TEST(test_malloc_small);
    AA_TEST(test_malloc_large);
    AA_TEST(test_malloc_many);
    AA_TEST(test_free_null_is_noop);
    AA_TEST(test_calloc_zeroed);
    AA_TEST(test_realloc_preserves_data);
    AA_TEST(test_verify_after_ops);
    return aa_test_summary();
}
