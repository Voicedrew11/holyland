/* The function profiler's counts (port/include/port/sample_table.h).

   The stacks are kept in an array, their frames in a pool of their own (a stack's are a run in it), found again
   by an open-addressed table of hashes. */
#include <stdlib.h>
#include <string.h>

#include "port/sample_table.h"

typedef struct {
    uint64_t hash;
    uint64_t count;
    size_t first; /* in the frame pool */
    int tid;
    int frame_count;
} Stack;

struct SampleTable {
    Stack *stacks;
    size_t stack_count, stack_room;
    uint64_t *frames;
    size_t frame_count, frame_room;
    size_t *slots; /* the stack + 1; 0: empty */
    size_t slot_count; /* a power of two, at least twice the stacks */
};

static int grow(void **array, size_t *room, size_t want, size_t item) {
    size_t room_now = *room ? *room : 256;
    void *bigger;

    if (want <= *room) {
        return 1;
    }
    while (room_now < want) {
        room_now *= 2;
    }
    bigger = realloc(*array, room_now * item);
    if (bigger == NULL) {
        return 0;
    }
    *array = bigger;
    *room = room_now;
    return 1;
}

static uint64_t hash_stack(int tid, const uint64_t *frames, int count) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(uint32_t)tid;
    int i;

    for (i = 0; i < count; i++) {
        h = (h ^ frames[i]) * 0x100000001b3ull;
    }
    return h ^ (h >> 29);
}

static size_t slot_of(const SampleTable *table, uint64_t hash) {
    return (size_t)hash & (table->slot_count - 1);
}

/* the slots made anew at twice the stacks' room (all stacks put back) */
static int rehash(SampleTable *table, size_t slot_count) {
    size_t *slots = calloc(slot_count, sizeof(*slots));
    size_t i;

    if (slots == NULL) {
        return 0;
    }
    free(table->slots);
    table->slots = slots;
    table->slot_count = slot_count;
    for (i = 0; i < table->stack_count; i++) {
        size_t at = slot_of(table, table->stacks[i].hash);

        while (table->slots[at] != 0) {
            at = (at + 1) & (slot_count - 1);
        }
        table->slots[at] = i + 1;
    }
    return 1;
}

SampleTable *sample_table_create(void) {
    SampleTable *table = calloc(1, sizeof(*table));

    if (table == NULL) {
        return NULL;
    }
    if (!rehash(table, 1024)) {
        free(table);
        return NULL;
    }
    return table;
}

void sample_table_destroy(SampleTable *table) {
    if (table == NULL) {
        return;
    }
    free(table->stacks);
    free(table->frames);
    free(table->slots);
    free(table);
}

void sample_table_clear(SampleTable *table) {
    table->stack_count = 0;
    table->frame_count = 0;
    memset(table->slots, 0, table->slot_count * sizeof(*table->slots));
}

static int same_stack(const SampleTable *table, const Stack *s, uint64_t hash, int tid, const uint64_t *frames,
                      int count) {
    return s->hash == hash && s->tid == tid && s->frame_count == count &&
           memcmp(table->frames + s->first, frames, (size_t)count * sizeof(*frames)) == 0;
}

int sample_table_add(SampleTable *table, int tid, const uint64_t *frames, int count) {
    uint64_t hash;
    size_t at;
    Stack *s;

    if (count < 0) {
        count = 0;
    }
    if (count > SAMPLE_FRAMES) {
        count = SAMPLE_FRAMES;
    }
    hash = hash_stack(tid, frames, count);
    at = slot_of(table, hash);
    while (table->slots[at] != 0) {
        s = &table->stacks[table->slots[at] - 1];
        if (same_stack(table, s, hash, tid, frames, count)) {
            s->count++;
            return 1;
        }
        at = (at + 1) & (table->slot_count - 1);
    }
    if (!grow((void **)&table->stacks, &table->stack_room, table->stack_count + 1, sizeof(Stack)) ||
        !grow((void **)&table->frames, &table->frame_room, table->frame_count + (size_t)count, sizeof(uint64_t))) {
        return 0;
    }
    s = &table->stacks[table->stack_count];
    s->hash = hash;
    s->count = 1;
    s->first = table->frame_count;
    s->tid = tid;
    s->frame_count = count;
    if (count > 0) {
        memcpy(table->frames + table->frame_count, frames, (size_t)count * sizeof(*frames));
    }
    table->frame_count += (size_t)count;
    table->stack_count++;
    table->slots[at] = table->stack_count;
    if (table->stack_count * 2 > table->slot_count) {
        /* (a rehash that fails leaves the old slots, still right, only fuller) */
        rehash(table, table->slot_count * 2);
    }
    return 1;
}

uint64_t sample_table_thread_samples(const SampleTable *table, int tid) {
    uint64_t n = 0;
    size_t i;

    for (i = 0; i < table->stack_count; i++) {
        if (table->stacks[i].tid == tid) {
            n += table->stacks[i].count;
        }
    }
    return n;
}

void sample_table_each(const SampleTable *table, SampleStackVisit visit, void *context) {
    size_t i;

    for (i = 0; i < table->stack_count; i++) {
        const Stack *s = &table->stacks[i];

        visit(context, s->tid, s->count, table->frames + s->first, s->frame_count);
    }
}

/* ---- A thread's functions ------------------------------------------------------------------- */
typedef struct {
    SampleFunctionTotal *totals;
    size_t count, room;
    size_t *slots; /* the total + 1 */
    size_t slot_count;
} Totals;

static uint64_t hash_function(uint64_t id) {
    uint64_t h = id * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 31);
}

static int totals_rehash(Totals *t, size_t slot_count) {
    size_t *slots = calloc(slot_count, sizeof(*slots));
    size_t i;

    if (slots == NULL) {
        return 0;
    }
    free(t->slots);
    t->slots = slots;
    t->slot_count = slot_count;
    for (i = 0; i < t->count; i++) {
        size_t at = (size_t)hash_function(t->totals[i].function) & (slot_count - 1);

        while (t->slots[at] != 0) {
            at = (at + 1) & (slot_count - 1);
        }
        t->slots[at] = i + 1;
    }
    return 1;
}

/* id's total, made when new; NULL when out of memory */
static SampleFunctionTotal *total_of(Totals *t, uint64_t id) {
    size_t at = (size_t)hash_function(id) & (t->slot_count - 1);
    SampleFunctionTotal *total;

    while (t->slots[at] != 0) {
        total = &t->totals[t->slots[at] - 1];
        if (total->function == id) {
            return total;
        }
        at = (at + 1) & (t->slot_count - 1);
    }
    if (!grow((void **)&t->totals, &t->room, t->count + 1, sizeof(*t->totals))) {
        return NULL;
    }
    total = &t->totals[t->count];
    total->function = id;
    total->self = 0;
    total->inclusive = 0;
    t->slots[at] = ++t->count;
    if (t->count * 2 > t->slot_count && !totals_rehash(t, t->slot_count * 2)) {
        return NULL;
    }
    return &t->totals[t->count - 1];
}

static int by_inclusive(const void *a, const void *b) {
    const SampleFunctionTotal *x = a, *y = b;

    if (x->inclusive != y->inclusive) {
        return x->inclusive > y->inclusive ? -1 : 1;
    }
    if (x->function != y->function) {
        return x->function < y->function ? -1 : 1;
    }
    return 0;
}

/* a stack's functions, each once (at most SAMPLE_FRAMES): their count */
static int stack_functions(const uint64_t *frames, int frame_count, SampleFunctionOf function_of, void *context,
                           uint64_t *ids) {
    int n = 0, i, j;

    for (i = 0; i < frame_count; i++) {
        uint64_t id = function_of(context, i == 0 ? frames[i] : frames[i] - 1);

        for (j = 0; j < n && ids[j] != id; j++) {
        }
        if (j == n) {
            ids[n++] = id;
        }
    }
    return n;
}

SampleFunctionTotal *sample_table_functions(const SampleTable *table, int tid, SampleFunctionOf function_of,
                                            void *context, size_t *count) {
    Totals t = {0};
    uint64_t ids[SAMPLE_FRAMES];
    size_t i;
    int ok = totals_rehash(&t, 256);

    *count = 0;
    for (i = 0; ok && i < table->stack_count; i++) {
        const Stack *s = &table->stacks[i];
        const uint64_t *frames = table->frames + s->first;
        int n, j;

        if (s->tid != tid || s->frame_count == 0) {
            continue;
        }
        n = stack_functions(frames, s->frame_count, function_of, context, ids);
        for (j = 0; ok && j < n; j++) {
            SampleFunctionTotal *total = total_of(&t, ids[j]);

            if (total == NULL) {
                ok = 0;
                break;
            }
            total->inclusive += s->count;
            if (j == 0) {
                total->self += s->count;
            }
        }
    }
    free(t.slots);
    if (!ok || t.count == 0) {
        free(t.totals);
        return NULL;
    }
    qsort(t.totals, t.count, sizeof(*t.totals), by_inclusive);
    *count = t.count;
    return t.totals;
}
