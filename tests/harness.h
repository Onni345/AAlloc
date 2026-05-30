/*
 * tests/harness.h — Minimal unit-test harness.
 *
 * Usage:
 *   int main(void) {
 *       AA_TEST(my_test_fn);
 *       return aa_test_summary();
 *   }
 */

#ifndef AALLOC_TEST_HARNESS_H
#define AALLOC_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>

static int _run = 0, _passed = 0, _failed = 0;

#define AA_EXPECT(cond) do { \
    _run++; \
    if (cond) { _passed++; } \
    else { _failed++; fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define AA_EXPECT_NULL(p)    AA_EXPECT((p) == NULL)
#define AA_EXPECT_NOTNULL(p) AA_EXPECT((p) != NULL)
#define AA_EXPECT_EQ(a, b)   AA_EXPECT((a) == (b))

#define AA_TEST(fn) do { \
    int _f0 = _failed; \
    fprintf(stderr, "[ RUN  ] %s\n", #fn); \
    fn(); \
    fprintf(stderr, _failed == _f0 ? "[ PASS ] %s\n" : "[ FAIL ] %s\n", #fn); \
} while (0)

static inline int aa_test_summary(void) {
    fprintf(stderr, "\n%d/%d passed%s\n", _passed, _run,
            _failed ? " — FAILURES DETECTED" : "");
    return _failed ? 1 : 0;
}

#endif /* AALLOC_TEST_HARNESS_H */
