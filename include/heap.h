/*
 * heap.h - a malloc/free/realloc implementation over a fixed-size arena.
 *
 * Every heap owns one contiguous arena obtained once at creation time.
 * Blocks carry a header and a footer ("boundary tags") so that neighbours
 * can be found in O(1) and merged on free. Free blocks are additionally
 * linked into an explicit doubly linked list so allocation only has to
 * look at free memory.
 */
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/* Payload alignment guaranteed by heap_alloc (same as glibc on x86-64). */
#define HEAP_ALIGNMENT 16

typedef enum {
    FIT_FIRST, /* first free block in list order that is large enough   */
    FIT_NEXT,  /* like first fit, but resume where the last search ended */
    FIT_BEST   /* smallest free block that is large enough              */
} fit_policy;

typedef struct heap heap_t;

typedef struct {
    size_t capacity;      /* bytes available for blocks (headers included) */
    size_t used_bytes;    /* bytes in allocated blocks, headers included   */
    size_t free_bytes;    /* bytes in free blocks, headers included        */
    size_t largest_free;  /* size of the largest free block                */
    size_t alloc_blocks;  /* number of allocated blocks                    */
    size_t free_blocks;   /* number of free blocks                         */
} heap_stats;

/* Create a heap managing roughly `arena_bytes` bytes. Returns NULL if the
 * size is too small (< 256 bytes) or the arena cannot be obtained. */
heap_t *heap_create(size_t arena_bytes, fit_policy policy);
void heap_destroy(heap_t *h);

/* Same contract as malloc: NULL for size 0 or when no block fits. */
void *heap_alloc(heap_t *h, size_t size);

/* Free a pointer returned by heap_alloc/heap_realloc. NULL is a no-op.
 * Returns 0 on success, -1 if the pointer is outside the arena, misaligned
 * or not currently allocated (catches double frees). */
int heap_free(heap_t *h, void *ptr);

/* Same contract as realloc. Grows in place when the next block is free. */
void *heap_realloc(heap_t *h, void *ptr, size_t size);

/* Usable bytes in the block behind ptr (>= the size that was requested). */
size_t heap_usable_size(const heap_t *h, const void *ptr);

/* Walk every block and the free list and verify all invariants.
 * Returns NULL when the heap is consistent, otherwise a description of
 * the first problem found. */
const char *heap_check(const heap_t *h);

heap_stats heap_get_stats(const heap_t *h);
const char *fit_policy_name(fit_policy p);

#endif
