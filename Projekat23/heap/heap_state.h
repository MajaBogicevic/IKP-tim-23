#include <pthread.h>
#include "heap_internal.h"
#include "heap.h"

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
    void *sp;

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


