#include "heap.h"
#include "heap_internal.h"
#include <pthread.h>
#include <stdlib.h>

struct Heap {
    size_t segment_size_bytes;
    size_t gc_threshold_bytes;

    pthread_mutex_t lock;

    Segment* segments;
    BlockHeader* free_list;

    size_t allocated_bytes;

    void*** roots;
    size_t roots_count;
    size_t roots_capacity;
};

static Segment* segment_create(size_t size_bytes)
{
    Segment* seg = (Segment*)malloc(sizeof(Segment));
    if (!seg) {
        return NULL;
    }

    seg->mem = (unsigned char*)malloc(size_bytes);
    if (!seg->mem) {
        free(seg);
        return NULL;
    }

    seg->size = size_bytes;
    seg->next = NULL;
    return seg;
}

static void segment_destroy_all(Segment* seg)
{
    while (seg) {
        Segment* next = seg->next;
        free(seg->mem);
        free(seg);
        seg = next;
    }
}

static void free_list_push(BlockHeader** head, BlockHeader* block)
{
    block->next_free = *head;
    *head = block;
}

static void free_list_remove(BlockHeader** head, BlockHeader* prev, BlockHeader* cur)
{
    if (prev) {
        prev->next_free = cur->next_free;
    } else {
        *head = cur->next_free;
    }
    cur->next_free = NULL;
}

Heap* create_heap(size_t segment_size_bytes, size_t gc_threshold_bytes)
{
    Heap* h = (Heap*)calloc(1, sizeof(Heap));
    h->roots = NULL;
    h->roots_count = 0;
    h->roots_capacity = 0;

    if (!h) {
        return NULL;
    }
    h->segment_size_bytes = segment_size_bytes;
    h->gc_threshold_bytes = gc_threshold_bytes;

    if(pthread_mutex_init(&h->lock, NULL) != 0) {
        free(h);
        return NULL;
    }

    Segment* seg = segment_create(segment_size_bytes);
    if (!seg) {
        pthread_mutex_destroy(&h->lock);
        free(h);
        return NULL;
    }
    h->segments = seg;

    BlockHeader* block = (BlockHeader*)seg->mem;
    block->size = seg->size - sizeof(BlockHeader);
    block->magic = BLOCK_MAGIC;
    block->flags = BLOCK_FLAG_FREE;
    block->next_free = NULL;

    h->free_list = NULL;
    free_list_push(&h->free_list, block);

    return h;
}

void destroy_heap(Heap* h)
{
    if(!h) {
        return;
    }

    pthread_mutex_lock(&h->lock);
    segment_destroy_all(h->segments);
    h->segments = NULL;
    h->free_list = NULL;
    pthread_mutex_unlock(&h->lock);

    pthread_mutex_destroy(&h->lock);

    free(h->roots);
    h->roots = NULL;
    h->roots_count = 0;
    h->roots_capacity = 0;
    free(h);
}

void* alloc_heap(Heap* h, size_t size_bytes)
{
    if (!h || size_bytes == 0) {
        return NULL;
    }

    size_t req = heap_align_up(size_bytes);

    pthread_mutex_lock(&h->lock);

    BlockHeader* prev = NULL;
    BlockHeader* cur = h->free_list;

    while (cur) {
        if ((cur->flags & BLOCK_FLAG_FREE) && cur->size >= req) {
            break;
        }
        prev = cur;
        cur = cur->next_free;
    }

    if (!cur) {
        Segment* ns = segment_create(h->segment_size_bytes);
        if (!ns) {
            pthread_mutex_unlock(&h->lock);
            return NULL;
        }
        ns->next = h->segments;
        h->segments = ns;

        BlockHeader* nb = (BlockHeader*)(void*)ns->mem;
        nb->size = ns->size - sizeof(BlockHeader);
        nb->magic = BLOCK_MAGIC;
        nb->flags = BLOCK_FLAG_FREE;
        nb->next_free = NULL;
        free_list_push(&h->free_list, nb);

        prev = NULL;
        cur = h->free_list;
        while (cur) {
            if ((cur->flags & BLOCK_FLAG_FREE) && cur->size >= req) {
                break;
            }
            prev = cur;
            cur = cur->next_free;
        }

        if (!cur) {
            pthread_mutex_unlock(&h->lock);
            return NULL;
        }
    }

    free_list_remove(&h->free_list, prev, cur);

    size_t remaining = 0;
    if (cur->size > req) {
        remaining = cur->size - req;
    }

    if (remaining > sizeof(BlockHeader) + HEAP_ALIGNMENT) {
        unsigned char* payload = (unsigned char*)(void*)(cur + 1);
        BlockHeader* split = (BlockHeader*)(void*)(payload + req);
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

    void* out = (void*)(cur + 1);
    pthread_mutex_unlock(&h->lock);
    return out;
}

void free_heap(Heap* h, void* ptr)
{
    if(!h || !ptr) {
        return;
    }

    pthread_mutex_lock(&h->lock);
    BlockHeader* block = (BlockHeader*)ptr - 1;
    if (block->magic != BLOCK_MAGIC) {
        pthread_mutex_unlock(&h->lock);
        return;
    }

    if((block->flags & BLOCK_FLAG_FREE)!=0) {
        pthread_mutex_unlock(&h->lock);
        return;
    }
    
    block->flags |= BLOCK_FLAG_FREE;
    block->flags &= ~BLOCK_FLAG_MARK;
    h->allocated_bytes -= block->size;

    free_list_push(&h->free_list, block);
    pthread_mutex_unlock(&h->lock);
}

static int ptr_in_segment(const Segment* seg, const void* p)
{
    const unsigned char* start = seg->mem;
    const unsigned char* end   = seg->mem + seg->size;
    const unsigned char* x     = (const unsigned char*)p;
    return (x >= start) && (x < end);
}

static BlockHeader* block_from_payload(Heap* h, void* payload)
{
    if (!payload) {
        return NULL;
    }

    BlockHeader* b = ((BlockHeader*)payload) - 1;
    if (b->magic != BLOCK_MAGIC) {
        return NULL;
    }
    if (b->flags & BLOCK_FLAG_FREE) {
        return NULL;
    }

    Segment* s = h->segments;
    while (s) {
        if (ptr_in_segment(s, (void*)b)) {
            return b;
        }
        s = s->next;
    }

    return NULL;
}

typedef void (*block_visit_fn)(Heap* h, Segment* seg, BlockHeader* b, void* ctx);

static void for_each_block(Heap* h, block_visit_fn fn, void* ctx)
{
    Segment* s = h->segments;
    while (s) {
        unsigned char* cur = s->mem;
        unsigned char* end = s->mem + s->size;

        while (cur + sizeof(BlockHeader) <= end) {
            BlockHeader* b = (BlockHeader*)(void*)cur;

            if (b->magic != BLOCK_MAGIC) {
                break;
            }

            fn(h, s, b, ctx);

            size_t step = sizeof(BlockHeader) + b->size;
            cur += step;

            if (step == 0) {
                break;
            }
        }

        s = s->next;
    }
}


void collect_heap(Heap* h)
{
    (void)h;
}

int roots_add(Heap* h, void** slot)
{
    if(!h || !slot) {
        return -1;
    }

    pthread_mutex_lock(&h->lock);

    for(size_t i = 0; i < h->roots_count; i++) {
        if(h->roots[i] == slot) {
            pthread_mutex_unlock(&h->lock);
            return 0;
        }
    }

    if(h->roots_count == h->roots_capacity) {
        size_t new_capacity = (h->roots_capacity == 0) ? 16 : h->roots_capacity * 2;
        void*** new_roots = (void***)realloc(h->roots, new_capacity * sizeof(void**));
        if(!new_roots) {
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

int roots_remove(Heap* h, void** slot)
{
    if (!h || !slot) {
        return -1;
    }

    pthread_mutex_lock(&h->lock);

    for (size_t i = 0; i < h->roots_count; i++) {
        if (h->roots[i] == slot) {
            h->roots[i] = h->roots[h->roots_count - 1];
            h->roots_count--;
            pthread_mutex_unlock(&h->lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&h->lock);
    return 0;
}

int thread_register(Heap* h)
{
    (void)h;
    return 0;
}

int thread_unregister(Heap* h)
{
    (void)h;
    return 0;
}

void gc_safepoint(Heap* h)
{
    (void)h;
}
