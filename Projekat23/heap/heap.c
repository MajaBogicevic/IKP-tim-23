#include "heap.h"
#include "heap_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// #ifdef __APPLE__
// #include <pthread.h>
// #endif

_Static_assert(sizeof(size_t) == sizeof(void *), "size_t same size as pointer required");

typedef enum
{
    THREAD_RUNNING = 0,
    THREAD_PARKED = 1
} ThreadStatus;

typedef struct ThreadInfo
{
    pthread_t tid;
    ThreadStatus status;

    void *stack_lo;
    void *stack_hi;
    void *sp; // stack pointer u safepointu

    struct ThreadInfo *next;
} ThreadInfo;

struct Heap
{
    size_t segment_size_bytes;
    size_t gc_threshold_bytes;

    pthread_mutex_t lock;

    Segment *segments;
    BlockHeader *free_list;

    size_t allocated_bytes;

    void ***roots;
    size_t roots_count;
    size_t roots_capacity;

    ThreadInfo *threads;
    pthread_cond_t gc_cond;
    int gc_requested;
};

// NAPRAVI SEGMENT
static Segment *segment_create(size_t size_bytes)
{
    Segment *seg = (Segment *)malloc(sizeof(Segment));
    if (!seg)
    {
        return NULL;
    }

    seg->mem = (unsigned char *)malloc(size_bytes);
    if (!seg->mem)
    {
        free(seg);
        return NULL;
    }

    seg->size = size_bytes;
    seg->next = NULL;
    return seg;
}

// UNISTI SVE SEGMENTE
static void segment_destroy_all(Segment *seg)
{
    while (seg)
    {
        Segment *next = seg->next;
        free(seg->mem);
        free(seg);
        seg = next;
    }
}

// DODAJ BLOK NA SLOBODNU LISTU
static void free_list_push(BlockHeader **head, BlockHeader *block)
{
    block->next_free = *head;
    *head = block;
}

// UKLONI BLOK SA SLOBODNE LISTE
static void free_list_remove(BlockHeader **head, BlockHeader *prev, BlockHeader *cur)
{
    if (prev)
    {
        prev->next_free = cur->next_free;
    }
    else
    {
        *head = cur->next_free;
    }
    cur->next_free = NULL;
}

// KREIRAJ HEAP
Heap *create_heap(size_t segment_size_bytes, size_t gc_threshold_bytes)
{
    Heap *h = (Heap *)calloc(1, sizeof(Heap));
    if (!h)
    {
        return NULL;
    }

    h->roots = NULL;
    h->roots_count = 0;
    h->roots_capacity = 0;

    h->segment_size_bytes = segment_size_bytes;
    h->gc_threshold_bytes = gc_threshold_bytes;

    if (pthread_mutex_init(&h->lock, NULL) != 0)
    {
        free(h);
        return NULL;
    }

    Segment *seg = segment_create(segment_size_bytes);
    if (!seg)
    {
        pthread_mutex_destroy(&h->lock);
        free(h);
        return NULL;
    }
    h->segments = seg;

    BlockHeader *block = (BlockHeader *)seg->mem;
    block->size = seg->size - sizeof(BlockHeader);
    block->magic = BLOCK_MAGIC;
    block->flags = BLOCK_FLAG_FREE;
    block->next_free = NULL;

    h->free_list = NULL;
    free_list_push(&h->free_list, block);

    pthread_cond_init(&h->gc_cond, NULL);
    h->threads = NULL;
    h->gc_requested = 0;

    return h;
}

// UNISTI HEAP
void destroy_heap(Heap *h)
{
    if (!h)
    {
        return;
    }

    pthread_mutex_lock(&h->lock);
    segment_destroy_all(h->segments);
    h->segments = NULL;
    h->free_list = NULL;
    pthread_mutex_unlock(&h->lock);

    pthread_mutex_destroy(&h->lock);
    pthread_cond_destroy(&h->gc_cond);

    free(h->roots);
    h->roots = NULL;
    h->roots_count = 0;
    h->roots_capacity = 0;
    free(h);
}

// ALOKACIJA MEMORIJE
void *alloc_heap(Heap *h, size_t size_bytes)
{
    if (!h || size_bytes == 0)
    {
        return NULL;
    }

    gc_safepoint(h);

    size_t req = heap_align_up(size_bytes);

    pthread_mutex_lock(&h->lock);

    BlockHeader *prev = NULL;
    BlockHeader *cur = h->free_list;

    while (cur)
    {
        if ((cur->flags & BLOCK_FLAG_FREE) && cur->size >= req)
        {
            break;
        }
        prev = cur;
        cur = cur->next_free;
    }

    if (!cur)
    {
        Segment *nseg = segment_create(h->segment_size_bytes);
        if (!nseg)
        {
            pthread_mutex_unlock(&h->lock);
            return NULL;
        }
        nseg->next = h->segments;
        h->segments = nseg;

        BlockHeader *nb = (BlockHeader *)(void *)nseg->mem;
        nb->size = nseg->size - sizeof(BlockHeader);
        nb->magic = BLOCK_MAGIC;
        nb->flags = BLOCK_FLAG_FREE;
        nb->next_free = NULL;
        free_list_push(&h->free_list, nb);

        prev = NULL;
        cur = h->free_list;
        while (cur)
        {
            if ((cur->flags & BLOCK_FLAG_FREE) && cur->size >= req)
            {
                break;
            }
            prev = cur;
            cur = cur->next_free;
        }

        if (!cur)
        {
            pthread_mutex_unlock(&h->lock);
            return NULL;
        }
    }

    free_list_remove(&h->free_list, prev, cur);

    size_t remaining = 0;
    if (cur->size > req)
    {
        remaining = cur->size - req;
    }

    if (remaining > sizeof(BlockHeader) + HEAP_ALIGNMENT)
    {
        unsigned char *payload = (unsigned char *)(void *)(cur + 1);
        BlockHeader *split = (BlockHeader *)(void *)(payload + req);
        split->size = remaining - sizeof(BlockHeader);
        split->magic = BLOCK_MAGIC;
        split->flags = BLOCK_FLAG_FREE;
        split->next_free = NULL;

        free_list_push(&h->free_list, split);

        cur->size = req;
    }

    cur->flags &= ~BLOCK_FLAG_FREE;
    cur->flags &= ~BLOCK_FLAG_MARK;
    h->allocated_bytes += cur->size;

    void *out = (void *)(cur + 1);
    memset(out, 0, cur->size);
    pthread_mutex_unlock(&h->lock);
    return out;
}

// OSLOBODI MEMORIJU
void free_heap(Heap *h, void *ptr)
{
    if (!h || !ptr)
    {
        return;
    }

    pthread_mutex_lock(&h->lock);
    BlockHeader *block = (BlockHeader *)ptr - 1;
    if (block->magic != BLOCK_MAGIC)
    {
        pthread_mutex_unlock(&h->lock);
        return;
    }

    if ((block->flags & BLOCK_FLAG_FREE) != 0)
    {
        pthread_mutex_unlock(&h->lock);
        return;
    }

    block->flags |= BLOCK_FLAG_FREE;
    block->flags &= ~BLOCK_FLAG_MARK;
    if (h->allocated_bytes >= block->size)
    {
        h->allocated_bytes -= block->size;
    }
    else
    {
        h->allocated_bytes = 0;
    }

    free_list_push(&h->free_list, block);
    pthread_mutex_unlock(&h->lock);
}

// POMOCNE FUNKCIJE ZA GC
static int ptr_in_segment(const Segment *seg, const void *p)
{
    size_t start = (size_t)(const void *)seg->mem;
    size_t end = start + seg->size;
    size_t x = (size_t)p;
    return (x >= start) && (x < end);
}

static BlockHeader *block_from_payload(Heap *h, void *payload)
{
    if (!h || !payload)
    {
        return NULL;
    }

    BlockHeader *b = ((BlockHeader *)payload) - 1;

    Segment *seg = h->segments;
    int in_segment = 0;
    while (seg)
    {
        if (ptr_in_segment(seg, (void *)b))
        {
            in_segment = 1;
            break;
        }
        seg = seg->next;
    }

    if (!in_segment)
    {
        return NULL;
    }

    if (b->magic != BLOCK_MAGIC)
    {
        return NULL;
    }
    if (b->flags & BLOCK_FLAG_FREE)
    {
        return NULL;
    }

    return b;
}

typedef void (*block_visit_fn)(Heap *h, Segment *seg, BlockHeader *b, void *ctx);

static void for_each_block(Heap *h, block_visit_fn fn, void *ctx)
{
    Segment *seg = h->segments;
    while (seg)
    {
        unsigned char *cur = seg->mem;
        unsigned char *end = seg->mem + seg->size;

        while (cur + sizeof(BlockHeader) <= end)
        {
            BlockHeader *b = (BlockHeader *)(void *)cur;

            if (b->magic != BLOCK_MAGIC)
            {
                break;
            }

            fn(h, seg, b, ctx);

            size_t step = sizeof(BlockHeader) + b->size;
            if (step == 0 || cur + step > end)
            {
                break;
            }
            cur += step;
        }

        seg = seg->next;
    }
}

// MARK
typedef struct MarkStack
{
    BlockHeader **items;
    size_t len;
    size_t cap;
} MarkStack;

static int markstack_init(MarkStack *st)
{
    st->cap = 128;
    st->len = 0;
    st->items = (BlockHeader **)malloc(st->cap * sizeof(BlockHeader *));
    return st->items ? 0 : -1;
}

static void markstack_destroy(MarkStack *st)
{
    free(st->items);
    st->items = NULL;
    st->len = 0;
    st->cap = 0;
}

static void markstack_push(MarkStack *st, BlockHeader *b)
{
    if (st->len == st->cap)
    {
        size_t new_cap = st->cap * 2;
        BlockHeader **ns = (BlockHeader **)realloc(st->items, new_cap * sizeof(BlockHeader *));
        if (!ns)
        {
            return;
        }
        st->items = ns;
        st->cap = new_cap;
    }
    st->items[st->len++] = b;
}

static BlockHeader *markstack_pop(MarkStack *st)
{
    if (st->len == 0)
    {
        return NULL;
    }
    return st->items[--st->len];
}

static void try_mark(Heap *h, MarkStack *st, void *candidate)
{
    BlockHeader *b = block_from_payload(h, candidate);
    if (!b)
    {
        return;
    }

    if (b->flags & BLOCK_FLAG_MARK)
    {
        return;
    }
    b->flags |= BLOCK_FLAG_MARK;

    markstack_push(st, b);
}

// SWEEP
static void sweep(Heap *hh, Segment *seg, BlockHeader *b, void *ctx)
{
    (void)seg;

    size_t *freed = (size_t *)ctx;

    if (b->flags & BLOCK_FLAG_FREE)
    {
        b->flags &= ~BLOCK_FLAG_MARK;
        return;
    }

    if (b->flags & BLOCK_FLAG_MARK)
    {
        b->flags &= ~BLOCK_FLAG_MARK;
        return;
    }

    b->flags |= BLOCK_FLAG_FREE;

    if (hh->allocated_bytes >= b->size)
        hh->allocated_bytes -= b->size;
    else
        hh->allocated_bytes = 0;

    free_list_push(&hh->free_list, b);
    if (freed)
    {
        (*freed)++;
    }
}

// GC
void collect_heap(Heap *h)
{
    if (!h)
    {
        return;
    }

    pthread_mutex_lock(&h->lock);
    h->gc_requested = 1;

    MarkStack st;
    if (markstack_init(&st) != 0)
    {
        pthread_mutex_unlock(&h->lock);
        return;
    }

    for (size_t i = 0; i < h->roots_count; i++)
    {
        void **slot = h->roots[i];
        if (!slot)
        {
            continue;
        }
        try_mark(h, &st, *slot);
    }

    ThreadInfo *ti = h->threads;
    while (ti)
    {
        if (ti->sp)
        {
            size_t *p = (size_t *)ti->sp;
            size_t *end = (size_t *)ti->stack_hi;

            for (; p < end; p++)
            {
                try_mark(h, &st, (void *)(*p));
            }
        }
        ti = ti->next;
    }

    for (;;)
    {
        BlockHeader *b = markstack_pop(&st);
        if (!b)
        {
            break;
        }

        size_t *words = (size_t *)(void *)(b + 1);
        size_t n = b->size / sizeof(size_t);

        for (size_t k = 0; k < n; k++)
        {
            void *cand = (void *)words[k];
            try_mark(h, &st, cand);
        }
    }

    int done;
    do
    {
        done = 1;
        ThreadInfo *ti = h->threads;
        while (ti)
        {
            if (!pthread_equal(ti->tid, pthread_self()) &&
                ti->status != THREAD_PARKED)
            {
                done = 0;
                break;
            }
            ti = ti->next;
        }
        if (!done)
            pthread_cond_wait(&h->gc_cond, &h->lock);
    } while (!done);

    markstack_destroy(&st);

    size_t freed = 0;
    for_each_block(h, sweep, &freed);

    h->gc_requested = 0;
    pthread_cond_broadcast(&h->gc_cond);

    pthread_mutex_unlock(&h->lock);
}

// POMOCNE FUNKCIJE ZA KORISNIKA HEAPA
int roots_add(Heap *h, void **slot)
{
    if (!h || !slot)
    {
        return -1;
    }

    pthread_mutex_lock(&h->lock);

    for (size_t i = 0; i < h->roots_count; i++)
    {
        if (h->roots[i] == slot)
        {
            pthread_mutex_unlock(&h->lock);
            return 0;
        }
    }

    if (h->roots_count == h->roots_capacity)
    {
        size_t new_capacity = (h->roots_capacity == 0) ? 16 : h->roots_capacity * 2;
        void ***new_roots = (void ***)realloc(h->roots, new_capacity * sizeof(void **));
        if (!new_roots)
        {
            pthread_mutex_unlock(&h->lock);
            return -1;
        }
        h->roots = new_roots;
        h->roots_capacity = new_capacity;
    }

    h->roots[h->roots_count++] = slot;
    pthread_mutex_unlock(&h->lock);

    return 0;
}

int roots_remove(Heap *h, void **slot)
{
    if (!h || !slot)
    {
        return -1;
    }

    pthread_mutex_lock(&h->lock);

    for (size_t i = 0; i < h->roots_count; i++)
    {
        if (h->roots[i] == slot)
        {
            h->roots[i] = h->roots[h->roots_count - 1];
            h->roots_count--;
            pthread_mutex_unlock(&h->lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&h->lock);
    return -1;
}

// POMOCNE FUNKCIJE ZA RAD SA NITIMA
int thread_register(Heap *h)
{
    if (!h)
        return -1;

    ThreadInfo *ti = calloc(1, sizeof(ThreadInfo));
    if (!ti)
        return -1;

    ti->tid = pthread_self();
    ti->status = THREAD_RUNNING;

#ifdef __APPLE__
    void *stack_hi = pthread_get_stackaddr_np(ti->tid);
    size_t stack_size = pthread_get_stacksize_np(ti->tid);
    ti->stack_hi = stack_hi;
    ti->stack_lo = (char *)stack_hi - stack_size;
#endif

    pthread_mutex_lock(&h->lock);
    ti->next = h->threads;
    h->threads = ti;
    pthread_mutex_unlock(&h->lock);

    return 0;
}

int thread_unregister(Heap *h)
{
    if (!h)
        return -1;

    pthread_t self = pthread_self();

    pthread_mutex_lock(&h->lock);
    ThreadInfo **pp = &h->threads;
    while (*pp)
    {
        if (pthread_equal((*pp)->tid, self))
        {
            ThreadInfo *dead = *pp;
            *pp = dead->next;
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&h->lock);
    return 0;
}

void gc_safepoint(Heap *h)
{
    if (!h)
        return;

    pthread_mutex_lock(&h->lock);

    if (!h->gc_requested)
    {
        pthread_mutex_unlock(&h->lock);
        return;
    }

    pthread_t self = pthread_self();
    ThreadInfo *ti = h->threads;
    while (ti)
    {
        if (pthread_equal(ti->tid, self))
        {
            ti->status = THREAD_PARKED;
            ti->sp = __builtin_frame_address(0);
            break;
        }
        ti = ti->next;
    }

    pthread_cond_broadcast(&h->gc_cond);

    while (h->gc_requested)
    {
        pthread_cond_wait(&h->gc_cond, &h->lock);
    }

    if (ti)
        ti->status = THREAD_RUNNING;

    pthread_mutex_unlock(&h->lock);
}
