/*
 * trace_runner - replay an allocation trace against each fit policy and
 * report how well each one used the heap.
 *
 * Trace format, one operation per line ('#' starts a comment):
 *   a <id> <bytes>    allocate and remember the block under <id>
 *   r <id> <bytes>    realloc block <id>
 *   f <id>            free block <id>
 *
 * Usage: trace_runner [-s heap_bytes] [-c] trace_file...
 *   -s  arena size in bytes (default 1048576)
 *   -c  run heap_check after every operation (slow, for debugging)
 */
#include "heap.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { char op; long id; size_t size; } trace_op;

typedef struct {
    trace_op *ops;
    size_t count;
    long max_id;
} trace_t;

static int load_trace(const char *path, trace_t *t) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    size_t cap = 1024;
    t->ops = malloc(cap * sizeof *t->ops);
    t->count = 0;
    t->max_id = -1;
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;
        trace_op op = {0};
        int n;
        if (*s == 'f') n = sscanf(s, "%c %ld", &op.op, &op.id) == 2;
        else n = sscanf(s, "%c %ld %zu", &op.op, &op.id, &op.size) == 3;
        if (!n || (op.op != 'a' && op.op != 'f' && op.op != 'r') || op.id < 0) {
            fprintf(stderr, "%s:%d: malformed line: %s", path, lineno, line);
            fclose(f); free(t->ops); return -1;
        }
        if (t->count == cap) {
            trace_op *grown = realloc(t->ops, 2 * cap * sizeof *t->ops);
            if (!grown) { fprintf(stderr, "out of memory\n"); fclose(f); free(t->ops); return -1; }
            t->ops = grown;
            cap *= 2;
        }
        t->ops[t->count++] = op;
        if (op.id > t->max_id) t->max_id = op.id;
    }
    fclose(f);
    return 0;
}

typedef struct {
    size_t failed;            /* alloc/realloc that returned NULL            */
    size_t peak_live;         /* max bytes requested and live at once        */
    double avg_ext_frag;      /* mean of 1 - largest_free/free_bytes         */
    double internal_frag;     /* (block bytes - requested) / block bytes, at peak */
    double ns_per_op;
    int check_error;
} result_t;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static result_t run(const trace_t *t, size_t heap_bytes, fit_policy p, int check) {
    result_t r = {0};
    heap_t *h = heap_create(heap_bytes, p);
    if (!h) { fprintf(stderr, "heap_create failed\n"); exit(1); }
    void **ptr = calloc((size_t)t->max_id + 1, sizeof *ptr);
    size_t *len = calloc((size_t)t->max_id + 1, sizeof *len);
    size_t live = 0, samples = 0;
    double frag_sum = 0;

    double start = now_ns();
    for (size_t i = 0; i < t->count; i++) {
        const trace_op *op = &t->ops[i];
        switch (op->op) {
        case 'a':
            if (ptr[op->id]) { live -= len[op->id]; heap_free(h, ptr[op->id]); }
            ptr[op->id] = heap_alloc(h, op->size);
            len[op->id] = ptr[op->id] ? op->size : 0;
            if (!ptr[op->id]) r.failed++; else live += op->size;
            break;
        case 'r': {
            void *q = heap_realloc(h, ptr[op->id], op->size);
            if (!q) { r.failed++; break; }
            live = live - len[op->id] + op->size;
            ptr[op->id] = q; len[op->id] = op->size;
            break;
        }
        case 'f':
            if (ptr[op->id]) { live -= len[op->id]; heap_free(h, ptr[op->id]); ptr[op->id] = NULL; len[op->id] = 0; }
            break;
        }
        if (check && heap_check(h)) { r.check_error = 1; break; }
        if (live > r.peak_live) {
            r.peak_live = live;
            heap_stats st = heap_get_stats(h);
            r.internal_frag = st.used_bytes ? 1.0 - (double)live / st.used_bytes : 0;
        }
    }
    double elapsed = now_ns() - start;
    r.ns_per_op = t->count ? elapsed / t->count : 0;

    /* Fragmentation is sampled in a second replay so the stats walk
     * (O(blocks)) does not pollute the timing above. */
    if (!check) {
        heap_destroy(h);
        h = heap_create(heap_bytes, p);
        memset(ptr, 0, ((size_t)t->max_id + 1) * sizeof *ptr);
        size_t every = t->count / 200 ? t->count / 200 : 1;
        for (size_t i = 0; i < t->count; i++) {
            const trace_op *op = &t->ops[i];
            if (op->op == 'a') { heap_free(h, ptr[op->id]); ptr[op->id] = heap_alloc(h, op->size); }
            else if (op->op == 'r') { void *q = heap_realloc(h, ptr[op->id], op->size); if (q) ptr[op->id] = q; }
            else { heap_free(h, ptr[op->id]); ptr[op->id] = NULL; }
            if (i % every == 0) {
                heap_stats st = heap_get_stats(h);
                if (st.free_bytes) { frag_sum += 1.0 - (double)st.largest_free / st.free_bytes; samples++; }
            }
        }
        r.avg_ext_frag = samples ? frag_sum / samples : 0;
    }

    free(ptr);
    free(len);
    heap_destroy(h);
    return r;
}

int main(int argc, char **argv) {
    size_t heap_bytes = 1 << 20;
    int check = 0, first_file = 1;
    for (; first_file < argc && argv[first_file][0] == '-'; first_file++) {
        if (!strcmp(argv[first_file], "-c")) check = 1;
        else if (!strcmp(argv[first_file], "-s") && first_file + 1 < argc) {
            char *end;
            heap_bytes = strtoull(argv[++first_file], &end, 10);
            if (*end || heap_bytes < 256) { fprintf(stderr, "bad -s value\n"); return 2; }
        } else {
            fprintf(stderr, "usage: %s [-s heap_bytes] [-c] trace_file...\n", argv[0]);
            return 2;
        }
    }
    if (first_file >= argc) {
        fprintf(stderr, "usage: %s [-s heap_bytes] [-c] trace_file...\n", argv[0]);
        return 2;
    }

    const fit_policy policies[] = {FIT_FIRST, FIT_NEXT, FIT_BEST};
    int status = 0;
    printf("heap size: %zu bytes%s\n\n", heap_bytes, check ? " (checking every op)" : "");
    printf("%-22s %-9s %8s %10s %9s %9s %8s\n",
           "trace", "policy", "ops", "failed", "ext-frag", "int-frag", "ns/op");
    for (int i = first_file; i < argc; i++) {
        trace_t t;
        if (load_trace(argv[i], &t)) { status = 1; continue; }
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        for (int p = 0; p < 3; p++) {
            result_t r = run(&t, heap_bytes, policies[p], check);
            if (r.check_error) {
                printf("%-22s %-9s heap_check FAILED\n", name, fit_policy_name(policies[p]));
                status = 1;
                continue;
            }
            if (check) /* fragmentation isn't sampled and timing is dominated by checks */
                printf("%-22s %-9s %8zu %10zu %9s %8.1f%% %8s\n",
                       name, fit_policy_name(policies[p]), t.count, r.failed,
                       "-", 100 * r.internal_frag, "-");
            else
                printf("%-22s %-9s %8zu %10zu %8.1f%% %8.1f%% %8.1f\n",
                       name, fit_policy_name(policies[p]), t.count, r.failed,
                       100 * r.avg_ext_frag, 100 * r.internal_frag, r.ns_per_op);
        }
        free(t.ops);
    }
    return status;
}
