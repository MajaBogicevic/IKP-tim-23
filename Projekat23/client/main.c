#include "../heap/heap.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

typedef struct
{
    Heap *h;
    size_t iters;
} WorkerArgs;

void *worker(void *arg)
{
    WorkerArgs *a = arg;
    thread_register(a->h);

    for (size_t i = 0; i < a->iters; i++)
    {
        void *p = alloc_heap(a->h, 64);
        free_heap(a->h, p);
    }

    thread_unregister(a->h);
    return NULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void run_bench(int nthreads)
{
    Heap *h = create_heap(1024 * 1024, 0);
    pthread_t t[nthreads];
    WorkerArgs a = {h, 1000000};

    double t0 = now_sec();
    for (int i = 0; i < nthreads; i++)
        pthread_create(&t[i], NULL, worker, &a);

    for (int i = 0; i < nthreads; i++)
        pthread_join(t[i], NULL);

    double t1 = now_sec();
    double ops = (double)(nthreads * a.iters) / (t1 - t0);

    printf("%d threads: %.0f ops/s\n", nthreads, ops);
    destroy_heap(h);
}

int main(void)
{
    Heap *h = create_heap(1024 * 1024, 0);

    void *a = alloc_heap(h, 64);
    void *b = alloc_heap(h, 64);

    memcpy(b, &a, sizeof(void *));

    roots_add(h, &b);
    collect_heap(h);

    void *c = alloc_heap(h, 64);
    printf("a=%p b=%p c=%p\n", a, b, c);

    run_bench(1);
    run_bench(2);
    run_bench(5);
    run_bench(10);

    destroy_heap(h);
    return 0;
}
