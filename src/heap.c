/*
 * heap.c - explicit free list allocator with boundary-tag coalescing.
 *
 * Arena layout (W = one 8-byte word):
 *
 *   | pad W | prologue hdr | prologue ftr | block | block | ... | epilogue hdr |
 *
 * Every block starts with a header word and ends with a footer word that
 * both hold (size | allocated-bit). Sizes are multiples of 16, so the low
 * four bits are free for flags. Block start addresses are 8 mod 16, which
 * makes every payload (block + 8) 16-byte aligned.
 *
 * Free blocks store two pointers at the start of their payload:
 *
 *   | hdr | next free | prev free | ...unused... | ftr |
 *
 * so the smallest possible block is 8 + 16 + 8 = 32 bytes.
 *
 * The prologue and epilogue are permanently "allocated" sentinels. They let
 * coalescing look at both neighbours without bounds checks.
 */
#include "heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WSIZE     sizeof(size_t)          /* header/footer word: 8 bytes   */
#define MIN_BLOCK ((size_t)32)            /* hdr + 2 pointers + ftr        */
#define ALLOC_BIT ((size_t)1)
#define SIZE_MASK (~(size_t)(HEAP_ALIGNMENT - 1))

struct heap {
    char *arena;       /* start of the memory we own                       */
    size_t arena_size; /* bytes in the arena (multiple of 16)              */
    char *first_block; /* first real block, right after the prologue       */
    char *epilogue;    /* address of the epilogue header                   */
    char *free_head;   /* head of the explicit free list (NULL if empty)   */
    char *rover;       /* next-fit resume point (a free block or NULL)     */
    fit_policy policy;
};

/* ---- word and block helpers ------------------------------------------- */

static inline size_t get_word(const char *p) { size_t w; memcpy(&w, p, WSIZE); return w; }
static inline void put_word(char *p, size_t w) { memcpy(p, &w, WSIZE); }

static inline size_t block_size(const char *b) { return get_word(b) & SIZE_MASK; }
static inline int is_alloc(const char *b) { return (int)(get_word(b) & ALLOC_BIT); }
static inline char *footer(char *b) { return b + block_size(b) - WSIZE; }
static inline char *next_block(char *b) { return b + block_size(b); }
static inline char *prev_block(char *b) { return b - (get_word(b - WSIZE) & SIZE_MASK); }
static inline char *payload(char *b) { return b + WSIZE; }
static inline char *block_of(void *p) { return (char *)p - WSIZE; }

static inline void set_block(char *b, size_t size, int alloc) {
    size_t w = size | (alloc ? ALLOC_BIT : 0);
    put_word(b, w);
    put_word(b + size - WSIZE, w);
}

/* Free-list links live in the payload of free blocks. */
static inline char *next_free(const char *b) { char *p; memcpy(&p, b + WSIZE, sizeof p); return p; }
static inline char *prev_free(const char *b) { char *p; memcpy(&p, b + 2 * WSIZE, sizeof p); return p; }
static inline void set_next_free(char *b, char *p) { memcpy(b + WSIZE, &p, sizeof p); }
static inline void set_prev_free(char *b, char *p) { memcpy(b + 2 * WSIZE, &p, sizeof p); }

/* Round a request up to a legal block size; 0 means "too large". */
static size_t adjust_size(size_t request) {
    if (request > SIZE_MAX - 2 * WSIZE - HEAP_ALIGNMENT) return 0;
    size_t size = (request + 2 * WSIZE + HEAP_ALIGNMENT - 1) & SIZE_MASK;
    return size < MIN_BLOCK ? MIN_BLOCK : size;
}

/* ---- free list --------------------------------------------------------- */

/* LIFO insertion: newly freed blocks go to the front. O(1). */
static void list_insert(heap_t *h, char *b) {
    set_prev_free(b, NULL);
    set_next_free(b, h->free_head);
    if (h->free_head) set_prev_free(h->free_head, b);
    h->free_head = b;
}

static void list_remove(heap_t *h, char *b) {
    char *prev = prev_free(b);
    char *next = next_free(b);
    if (prev) set_next_free(prev, next); else h->free_head = next;
    if (next) set_prev_free(next, prev);
    /* Never leave the next-fit rover pointing at a block that is no
     * longer free (it may be about to be allocated or merged away). */
    if (h->rover == b) h->rover = next;
}

/* ---- coalescing and placement ----------------------------------------- */

/* Merge a newly freed block (not yet in the list) with free neighbours,
 * insert the result into the free list and return it. The four classic
 * cases fall out of checking the previous footer and the next header. */
static char *coalesce(heap_t *h, char *b) {
    size_t size = block_size(b);
    char *prev = prev_block(b);
    char *next = next_block(b);
    int prev_alloc = is_alloc(prev);
    int next_alloc = is_alloc(next);

    if (!next_alloc) {               /* absorb the following block */
        list_remove(h, next);
        size += block_size(next);
    }
    if (!prev_alloc) {               /* get absorbed by the previous block */
        list_remove(h, prev);
        size += block_size(prev);
        b = prev;
    }
    set_block(b, size, 0);
    list_insert(h, b);
    return b;
}

/* Allocate `asize` bytes out of free block b, splitting off the tail if
 * the remainder is big enough to be a block of its own. */
static void place(heap_t *h, char *b, size_t asize) {
    size_t size = block_size(b);
    list_remove(h, b);
    if (size - asize >= MIN_BLOCK) {
        set_block(b, asize, 1);
        char *rest = b + asize;
        set_block(rest, size - asize, 0);
        list_insert(h, rest);
        if (h->policy == FIT_NEXT) h->rover = rest;
    } else {
        set_block(b, size, 1);
    }
}

/* ---- fit policies ----------------------------------------------------- */

static char *find_first(heap_t *h, size_t asize) {
    for (char *b = h->free_head; b; b = next_free(b))
        if (block_size(b) >= asize) return b;
    return NULL;
}

static char *find_next(heap_t *h, size_t asize) {
    char *start = h->rover ? h->rover : h->free_head;
    if (!start) return NULL;
    char *b = start;
    do {
        if (block_size(b) >= asize) { h->rover = b; return b; }
        b = next_free(b);
        if (!b) b = h->free_head;    /* wrap around */
    } while (b != start);
    return NULL;
}

static char *find_best(heap_t *h, size_t asize) {
    char *best = NULL;
    for (char *b = h->free_head; b; b = next_free(b)) {
        size_t s = block_size(b);
        if (s >= asize && (!best || s < block_size(best))) {
            best = b;
            if (s == asize) break;   /* can't do better than exact */
        }
    }
    return best;
}

static char *find_fit(heap_t *h, size_t asize) {
    switch (h->policy) {
    case FIT_NEXT: return find_next(h, asize);
    case FIT_BEST: return find_best(h, asize);
    case FIT_FIRST:
    default:       return find_first(h, asize);
    }
}

/* ---- public API ------------------------------------------------------- */

heap_t *heap_create(size_t arena_bytes, fit_policy policy) {
    if (arena_bytes < 256 || arena_bytes > SIZE_MAX / 2) return NULL;
    if (policy != FIT_FIRST && policy != FIT_NEXT && policy != FIT_BEST) return NULL;

    heap_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->arena_size = arena_bytes & SIZE_MASK;
    h->arena = aligned_alloc(HEAP_ALIGNMENT, h->arena_size);
    if (!h->arena) { free(h); return NULL; }
    h->policy = policy;

    char *a = h->arena;
    put_word(a, 0);                                   /* alignment pad   */
    set_block(a + WSIZE, 2 * WSIZE, 1);               /* prologue        */
    h->first_block = a + 3 * WSIZE;
    h->epilogue = a + h->arena_size - WSIZE;
    put_word(h->epilogue, 0 | ALLOC_BIT);             /* epilogue        */

    /* Everything in between is one big free block. */
    set_block(h->first_block, (size_t)(h->epilogue - h->first_block), 0);
    list_insert(h, h->first_block);
    return h;
}

void heap_destroy(heap_t *h) {
    if (!h) return;
    free(h->arena);
    free(h);
}

void *heap_alloc(heap_t *h, size_t size) {
    if (!h || size == 0) return NULL;
    size_t asize = adjust_size(size);
    if (asize == 0) return NULL;
    char *b = find_fit(h, asize);
    if (!b) return NULL;
    place(h, b, asize);
    return payload(b);
}

/* Is ptr a payload address of a currently allocated block? We can't verify
 * arbitrary interior pointers cheaply, but we can reject anything outside
 * the arena, misaligned, or whose header isn't marked allocated. */
static int valid_ptr(const heap_t *h, const void *ptr) {
    const char *p = ptr;
    if (p < h->first_block + WSIZE || p >= h->epilogue) return 0;
    if (((uintptr_t)p & (HEAP_ALIGNMENT - 1)) != 0) return 0;
    const char *b = p - WSIZE;
    size_t s = block_size(b);
    if (!is_alloc(b) || s < MIN_BLOCK || b + s > h->epilogue) return 0;
    return get_word(b) == get_word(b + s - WSIZE);
}

int heap_free(heap_t *h, void *ptr) {
    if (!ptr) return 0;
    if (!h || !valid_ptr(h, ptr)) return -1;
    char *b = block_of(ptr);
    set_block(b, block_size(b), 0);
    coalesce(h, b);
    return 0;
}

void *heap_realloc(heap_t *h, void *ptr, size_t size) {
    if (!h) return NULL;
    if (!ptr) return heap_alloc(h, size);
    if (size == 0) { heap_free(h, ptr); return NULL; }
    if (!valid_ptr(h, ptr)) return NULL;

    size_t asize = adjust_size(size);
    if (asize == 0) return NULL;
    char *b = block_of(ptr);
    size_t cur = block_size(b);

    /* Shrink (or same size): keep the block, give back the tail. */
    if (asize <= cur) {
        if (cur - asize >= MIN_BLOCK) {
            set_block(b, asize, 1);
            char *rest = b + asize;
            set_block(rest, cur - asize, 0);
            coalesce(h, rest);
        }
        return ptr;
    }

    /* Grow in place if the next block is free and large enough. */
    char *next = next_block(b);
    if (!is_alloc(next) && cur + block_size(next) >= asize) {
        size_t total = cur + block_size(next);
        list_remove(h, next);
        if (total - asize >= MIN_BLOCK) {
            set_block(b, asize, 1);
            char *rest = b + asize;
            set_block(rest, total - asize, 0);
            list_insert(h, rest);
        } else {
            set_block(b, total, 1);
        }
        return ptr;
    }

    /* Otherwise move. On failure the old block is left untouched. */
    void *fresh = heap_alloc(h, size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, cur - 2 * WSIZE);
    heap_free(h, ptr);
    return fresh;
}

size_t heap_usable_size(const heap_t *h, const void *ptr) {
    if (!h || !ptr || !valid_ptr(h, ptr)) return 0;
    return block_size((const char *)ptr - WSIZE) - 2 * WSIZE;
}

const char *heap_check(const heap_t *h) {
    if (!h) return "null heap";
    const char *pro = h->first_block - 2 * WSIZE;
    if (get_word(pro) != (2 * WSIZE | ALLOC_BIT) || get_word(pro + WSIZE) != get_word(pro))
        return "prologue corrupted";
    if (get_word(h->epilogue) != ALLOC_BIT) return "epilogue corrupted";

    size_t free_in_walk = 0;
    int prev_was_free = 0;
    char *b = h->first_block;
    while (b != h->epilogue) {
        if (b > h->epilogue) return "block walk ran past the epilogue";
        size_t s = block_size(b);
        if (s < MIN_BLOCK) return "block smaller than minimum size";
        if (s % HEAP_ALIGNMENT) return "block size not a multiple of 16";
        if (((uintptr_t)payload(b) & (HEAP_ALIGNMENT - 1)) != 0) return "payload misaligned";
        if (b + s > h->epilogue) return "block extends past the epilogue";
        if (get_word(b) != get_word(b + s - WSIZE)) return "header and footer disagree";
        if (!is_alloc(b)) {
            if (prev_was_free) return "two adjacent free blocks (missed coalesce)";
            free_in_walk++;
            prev_was_free = 1;
        } else {
            prev_was_free = 0;
        }
        b += s;
    }

    size_t free_in_list = 0;
    const char *prev = NULL;
    for (const char *f = h->free_head; f; f = next_free(f)) {
        if (f < h->first_block || f >= h->epilogue) return "free list pointer outside heap";
        if (is_alloc(f)) return "allocated block on the free list";
        if (prev_free(f) != prev) return "free list prev pointer inconsistent";
        if (++free_in_list > free_in_walk) return "free list has a cycle or stray entries";
        prev = f;
    }
    if (free_in_list != free_in_walk) return "free block missing from free list";
    if (h->rover && is_alloc(h->rover)) return "next-fit rover points at an allocated block";
    return NULL;
}

heap_stats heap_get_stats(const heap_t *h) {
    heap_stats st = {0};
    if (!h) return st;
    st.capacity = (size_t)(h->epilogue - h->first_block);
    for (char *b = h->first_block; b != h->epilogue; b = next_block(b)) {
        size_t s = block_size(b);
        if (is_alloc(b)) { st.used_bytes += s; st.alloc_blocks++; }
        else {
            st.free_bytes += s; st.free_blocks++;
            if (s > st.largest_free) st.largest_free = s;
        }
    }
    return st;
}

const char *fit_policy_name(fit_policy p) {
    switch (p) {
    case FIT_FIRST: return "first-fit";
    case FIT_NEXT:  return "next-fit";
    case FIT_BEST:  return "best-fit";
    }
    return "unknown";
}
