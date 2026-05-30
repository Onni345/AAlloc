/*
 * aalloc.h — Public API for the aalloc memory allocator.
 */

#ifndef AALLOC_H
#define AALLOC_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void *aa_malloc(size_t size);
void  aa_free(void *p);
void *aa_realloc(void *ptr, size_t size);
void *aa_calloc(size_t nmemb, size_t size);

/* Structural integrity check — returns true if the allocator state is valid. */
bool  aa_verify(void);

#ifdef __cplusplus
}
#endif

#endif /* AALLOC_H */
