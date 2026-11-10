/*
 * Unit and stress tests for the allocator. No external framework: each
 * test is a function, CHECK records failures, and main() runs every test
 * against every fit policy.
 */
#include "heap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;
static const char *current_test = "";
static fit_policy current_policy;

#define CHECK(cond) do {                                                      \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
        failures++;                                                           \
        fprintf(stderr, "FAIL [%s, %s] %s:%d: %s\n", current_test,            \
                fit_policy_name(current_policy), __FILE__, __LINE__, #cond);  \
    }                                                                         \
} while (0)

#define CHECK_HEAP(h) do {                                                    \
    const char *err_ = heap_check(h);                                         \
    checks++;                                                                 \
    if (err_) {                                                               \
        failures++;                                                           \
        fprintf(stderr, "FAIL [%s, %s] %s:%d: heap_check: %s\n",              \
                current_test, fit_policy_name(current_policy),                \
                __FILE__, __LINE__, err_);                                    \
    }                                                                         \
} while (0)

/* ---------------------------------------------------------------------- */

static void test_create_rejects_bad_input(fit_policy p) {
    CHECK(heap_create(0, p) == NULL);
    CHECK(heap_create(100, p) == NULL);
    CHECK(heap_create(4096, (fit_policy)42) == NULL);
}

static void test_fresh_heap_is_one_free_block(fit_policy p) {
    heap_t *h = heap_create(4096, p);
    CHECK(h != NULL);
    CHECK_HEAP(h);
    heap_stats st = heap_get_stats(h);
    CHECK(st.free_blocks == 1);
    CHECK(st.alloc_blocks == 0);
    CHECK(st.largest_free == st.capacity);
    CHECK(st.capacity == 4096 - 32); /* pad + prologue + epilogue */
    heap_destroy(h);
}

static void test_alignment_and_zero_size(fit_policy p) {
    heap_t *h = heap_create(1 << 16, p);
    CHECK(heap_alloc(h, 0) == NULL);
    for (size_t n = 1; n <= 200; n += 7) {
        void *q = heap_alloc(h, n);
        CHECK(q != NULL);
        CHECK(((uintptr_t)q % HEAP_ALIGNMENT) == 0);
        CHECK(heap_usable_size(h, q) >= n);
    }
    CHECK_HEAP(h);
    heap_destroy(h);
}

static void test_split_leaves_remainder_free(fit_policy p) {
    heap_t *h = heap_create(4096, p);
    void *a = heap_alloc(h, 100);
    heap_stats st = heap_get_stats(h);
    CHECK(a != NULL);
    CHECK(st.alloc_blocks == 1);
    CHECK(st.free_blocks == 1);
    CHECK(st.used_bytes == 128);  /* 100 + 16 overhead, rounded to 16 */
    CHECK(st.used_bytes + st.free_bytes == st.capacity);
    CHECK_HEAP(h);
    heap_destroy(h);
}

/* Free the middle of three blocks with each combination of neighbour
 * states and confirm the free-block count after coalescing. */
static void test_coalesce_four_cases(fit_policy p) {
    heap_t *h = heap_create(4096, p);
    void *a = heap_alloc(h, 64), *b = heap_alloc(h, 64), *c = heap_alloc(h, 64);
    void *guard = heap_alloc(h, 64); /* keeps c's right neighbour allocated */
    CHECK(a && b && c && guard);

    /* case 1: both neighbours allocated -> new isolated free block */
    CHECK(heap_free(h, b) == 0);
    CHECK(heap_get_stats(h).free_blocks == 2);
    CHECK_HEAP(h);

    /* case 2: left neighbour free -> merges with b's hole */
    CHECK(heap_free(h, c) == 0);
    CHECK(heap_get_stats(h).free_blocks == 2);
    CHECK_HEAP(h);

    /* case 3: right neighbour free -> a merges with the (b,c) hole */
    CHECK(heap_free(h, a) == 0);
    CHECK(heap_get_stats(h).free_blocks == 2);
    CHECK_HEAP(h);

    /* case 4: both neighbours free -> everything is one block again */
    CHECK(heap_free(h, guard) == 0);
    heap_stats st = heap_get_stats(h);
    CHECK(st.free_blocks == 1);
    CHECK(st.largest_free == st.capacity);
    CHECK_HEAP(h);
    heap_destroy(h);
}

static void test_freed_memory_is_reused(fit_policy p) {
    heap_t *h = heap_create(4096, p);
    void *a = heap_alloc(h, 200);
    void *b = heap_alloc(h, 200);
    (void)b;
    heap_free(h, a);
    void *c = heap_alloc(h, 200);
    /* First and best fit return the exact-size hole. Next fit resumes at
     * the big remainder after b, which is the point of that policy. */
    if (p == FIT_NEXT) CHECK(c != NULL && c != a);
    else CHECK(c == a);
    CHECK_HEAP(h);
    heap_destroy(h);
}

static void test_out_of_memory(fit_policy p) {
    heap_t *h = heap_create(1024, p);
    CHECK(heap_alloc(h, 2000) == NULL);
    CHECK(heap_alloc(h, SIZE_MAX) == NULL);
    CHECK(heap_alloc(h, SIZE_MAX - 8) == NULL);
    int n = 0;
    while (heap_alloc(h, 48)) n++;
    CHECK(n > 0);
    CHECK(heap_alloc(h, 48) == NULL);
    CHECK_HEAP(h);
    heap_destroy(h);
}

static void test_invalid_and_double_free(fit_policy p) {
    heap_t *h = heap_create(4096, p);
    int local;
    void *a = heap_alloc(h, 64);
    CHECK(heap_free(h, NULL) == 0);
    CHECK(heap_free(h, &local) == -1);          /* not from this heap  */
    CHECK(heap_free(h, (char *)a + 1) == -1);   /* misaligned          */
    CHECK(heap_free(h, a) == 0);
    CHECK(heap_free(h, a) == -1);               /* double free         */
    CHECK_HEAP(h);
    heap_destroy(h);
}

static void test_realloc_semantics(fit_policy p) {
    heap_t *h = heap_create(1 << 14, p);

    /* NULL behaves like alloc, size 0 behaves like free */
    char *a = heap_realloc(h, NULL, 40);
    CHECK(a != NULL);
    memset(a, 'x', 40);

    /* grow in place: the block after a is the free remainder */
    char *g = heap_realloc(h, a, 400);
    CHECK(g == a);
    for (int i = 0; i < 40; i++) CHECK(g[i] == 'x');
    CHECK_HEAP(h);

    /* shrink keeps the address and returns the tail */
    size_t before = heap_get_stats(h).used_bytes;
    char *s = heap_realloc(h, g, 32);
    CHECK(s == g);
    CHECK(heap_get_stats(h).used_bytes < before);
    CHECK_HEAP(h);

    /* blocked on the right -> must move, contents preserved */
    char *wall = heap_alloc(h, 32);
    CHECK(wall != NULL);
    memset(s, 'y', 32);
    char *m = heap_realloc(h, s, 1000);
    CHECK(m != NULL && m != s);
    for (int i = 0; i < 32; i++) CHECK(m[i] == 'y');
    CHECK_HEAP(h);

    /* failure leaves the original intact */
    char *f = heap_realloc(h, m, 1 << 20);
    CHECK(f == NULL);
    CHECK(m[0] == 'y');
    CHECK_HEAP(h);

    CHECK(heap_realloc(h, m, 0) == NULL);
    CHECK_HEAP(h);
    heap_destroy(h);
}

/* Random alloc/free/realloc against a shadow table. Every live block is
 * filled with a byte derived from its slot, so any overlap between blocks
 * or corruption by the allocator's own metadata shows up as a mismatch. */
#define SLOTS 256
static void test_random_stress(fit_policy p) {
    heap_t *h = heap_create(1 << 18, p);
    unsigned char *ptr[SLOTS] = {0};
    size_t len[SLOTS] = {0};
    srand(12345u + (unsigned)p);

    int ops = 20000, corrupt = 0, check_failed = 0, oom = 0;
    for (int i = 0; i < ops; i++) {
        int s = rand() % SLOTS;
        int action = rand() % 3;
        if (!ptr[s]) {
            size_t n = (size_t)(rand() % 2000) + 1;
            ptr[s] = heap_alloc(h, n);
            if (ptr[s]) { len[s] = n; memset(ptr[s], s & 0xFF, n); } else oom++;
        } else if (action == 0) {
            for (size_t k = 0; k < len[s]; k++) if (ptr[s][k] != (s & 0xFF)) { corrupt++; break; }
            heap_free(h, ptr[s]);
            ptr[s] = NULL;
        } else if (action == 1) {
            size_t n = (size_t)(rand() % 3000) + 1;
            unsigned char *q = heap_realloc(h, ptr[s], n);
            if (q) {
                size_t keep = n < len[s] ? n : len[s];
                for (size_t k = 0; k < keep; k++) if (q[k] != (s & 0xFF)) { corrupt++; break; }
                memset(q, s & 0xFF, n);
                ptr[s] = q; len[s] = n;
            } else oom++;
        }
        if (heap_check(h)) { check_failed++; break; }
    }
    for (int s = 0; s < SLOTS; s++) {
        if (!ptr[s]) continue;
        for (size_t k = 0; k < len[s]; k++) if (ptr[s][k] != (s & 0xFF)) { corrupt++; break; }
        heap_free(h, ptr[s]);
    }
    CHECK(corrupt == 0);
    CHECK(check_failed == 0);
    CHECK_HEAP(h);
    heap_stats st = heap_get_stats(h);
    CHECK(st.free_blocks == 1 && st.largest_free == st.capacity); /* all coalesced back */
    (void)oom;
    heap_destroy(h);
}

/* Best fit should pick the tighter hole; first fit takes the first one. */
static void test_policy_choice(fit_policy p) {
    heap_t *h = heap_create(1 << 14, p);
    void *big = heap_alloc(h, 500);
    void *sep1 = heap_alloc(h, 16);
    void *small = heap_alloc(h, 100);
    void *sep2 = heap_alloc(h, 16);
    CHECK(big && sep1 && small && sep2);
    heap_free(h, small);  /* free list (LIFO): small, ...      */
    heap_free(h, big);    /* free list (LIFO): big, small, ... */
    void *q = heap_alloc(h, 100);
    if (p == FIT_BEST) CHECK(q == small);
    if (p == FIT_FIRST) CHECK(q == big);
    CHECK_HEAP(h);
    heap_destroy(h);
}

/* ---------------------------------------------------------------------- */

typedef void (*test_fn)(fit_policy);
static const struct { const char *name; test_fn fn; } tests[] = {
    {"create_rejects_bad_input", test_create_rejects_bad_input},
    {"fresh_heap_is_one_free_block", test_fresh_heap_is_one_free_block},
    {"alignment_and_zero_size", test_alignment_and_zero_size},
    {"split_leaves_remainder_free", test_split_leaves_remainder_free},
    {"coalesce_four_cases", test_coalesce_four_cases},
    {"freed_memory_is_reused", test_freed_memory_is_reused},
    {"out_of_memory", test_out_of_memory},
    {"invalid_and_double_free", test_invalid_and_double_free},
    {"realloc_semantics", test_realloc_semantics},
    {"random_stress", test_random_stress},
    {"policy_choice", test_policy_choice},
};

int main(void) {
    const fit_policy policies[] = {FIT_FIRST, FIT_NEXT, FIT_BEST};
    size_t n = sizeof tests / sizeof tests[0];
    for (size_t pi = 0; pi < 3; pi++) {
        current_policy = policies[pi];
        for (size_t i = 0; i < n; i++) {
            current_test = tests[i].name;
            int before = failures;
            tests[i].fn(current_policy);
            printf("%-4s %-30s %s\n", failures == before ? "ok" : "FAIL",
                   tests[i].name, fit_policy_name(current_policy));
        }
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
