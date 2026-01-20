#include "heap_state.h"
#include <stdlib.h>
#include <string.h>

static void free_list_push(BlockHeader **head, BlockHeader *block)
{
    block->next_free = *head;
    *head = block;
}

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
