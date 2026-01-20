#include "heap_state.h"
#include <stdlib.h>
#include <pthread.h>

static void free_list_push(BlockHeader **head, BlockHeader *block)
{
    block->next_free = *head;
    *head = block;
}


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

// ------ MARK -----------
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


// -------- SWEEP -----------
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

//------ GARBAJE COLLECTOR ------
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

    BlockHeader *b;
    while ((b = markstack_pop(&st)) != NULL)
    {
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