#include "../heap/heap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <mach/mach.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <stdint.h>

//za ispis zauzete RAM memorije
static double rss_mb(void)
{
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return -1.0;

    return (double)info.resident_size / (1024.0 * 1024.0);
}

//segment test
static void run_segment_growth_test(void)
{
    printf("\n--------- TEST 2: SEGMENT ----------\n");

    Heap *h = create_heap(64 * 1024, 0);
    if (h == NULL) {
        return;
    }

    const int N = 800;
    void *ptrs[N];

    for (int i = 0; i < N; i++) {
        ptrs[i] = alloc_heap(h, 1024);
        if(ptrs[i] == NULL){
            printf("FAIL: alloc_heap fail i=%d\n", i);
            destroy_heap(h);
            return;
        }
        if (i % 50 == 0) {
            printf("Alloc #%d ptr=%p\n", i, ptrs[i]);
        }
        memset(ptrs[i], 0xCD, 32);
    }

    printf("All allocations completed.\n");
    for (int i = 0; i < N; i++) ptrs[i] = NULL;
    collect_heap(h);

    destroy_heap(h);
    printf("Test 2 is completed.\n");
}

//mark sweep cuvanje roots kontainera
static void run_roots_gc_test(Heap *h)
{
    printf("\n-------- TEST 1: ROOTS AND MARK ---------\n");

    void *a = alloc_heap(h, 64);
    if (!a) {
        printf("FAIL: alloc_heap a\n");
        return;
    }
    memset(a, 0xAB, 64);

    void *b = alloc_heap(h, sizeof(void*));
    if (!b) {
        printf("FAIL: alloc_heap b\n");
        free_heap(h, a);
        return;
    }
    memcpy(b, &a, sizeof(void*));

    int rc = roots_add(h, &b);
    if (rc != 0) {
        printf("FAIL: roots_add b\n");
        free_heap(h, b);
        free_heap(h, a);
        return;
    }    
    printf("Roots added.\n");
    collect_heap(h);

    for (int i = 0; i < 64; i++) {
        if (((unsigned char*)a)[i] != 171) {
            printf("FAIL: a[%d] occupied after GC\n", i);
            roots_remove(h, &b);
            return;
        }
    }

    rc = roots_remove(h, &b);
    if (rc != 0) {
        printf("FAIL: roots_remove b\n");
        return;
    }

    a = NULL;
    b = NULL;
    collect_heap(h);

    printf("Test 1 is completed.\n");
}

// vise niti baratanje sa velikom kolicinom memorije
typedef struct {
    int id;
    unsigned int seed;
    unsigned long long ops;
    unsigned long long alloc_ok;
    unsigned long long alloc_fail;
    unsigned long long explicit_frees;

} WorkerArgs; //niti

static Heap *g_heap = NULL;
static atomic_int g_stop = 0;

static size_t g_msg_bytes = 256 * 1024; 
static int g_window = 4;     

//heap gc za vise niti
static void *stress_worker(void *arg)
{
    WorkerArgs *a = (WorkerArgs*)arg;
    a->alloc_ok = 0;
    a->alloc_fail = 0;
    a->explicit_frees = 0;

    thread_register(g_heap);

    void *win[64]; //cuva pokazivace na alocirane blokove
    int W = g_window; //velicina prozora
    if (W > 64) W = 64;
    for (int i = 0; i < W; i++) win[i] = NULL;

    unsigned long long cnt = 0;
    unsigned long long idx = 0;

    while (!atomic_load(&g_stop)) {
        void *p = alloc_heap(g_heap, g_msg_bytes);
        if (p) {
            a->alloc_ok++;
            memset(p, (unsigned char)(a->id + 1), 64);
            win[idx % (unsigned long long)W] = p;
            idx++;

            if ((cnt % 256) == 0) {
                for (int j = 0; j < W / 2; j++) {
                    win[(idx + (unsigned long long)j) % (unsigned long long)W] = NULL;
                }
            }

            if ((cnt % 512) == 0) {
                int k = (int)((idx + 3) % (unsigned long long)W);
                void *f = win[k];
                win[k] = NULL;
                if (f) {
                    free_heap(g_heap, f);
                    a->explicit_frees++;
                }
            }
        } else{
            a->alloc_fail++;
        }

        if ((cnt % 1024) == 0) {
            int ms = (int)(rand_r(&a->seed) % 5u);
            usleep((useconds_t)(ms * 1000));
        }

        cnt++;
    }

    for (int i = 0; i < W; i++) win[i] = NULL;

    thread_unregister(g_heap);

    a->ops = cnt;
    return NULL;
}

static void run_multithread_bigmem_test(int nthreads, int seconds, size_t msg_bytes, int window)
{
    printf("\n----------- TEST 3: MULTITHREAD BIG MEMORY STRESS -----------\n");
    printf("threads=%d, seconds=%d, msg=%zu KB, window=%d\n", nthreads, seconds, msg_bytes / 1024, window);

    g_heap = create_heap(1024 * 1024, 0);
    if(g_heap == NULL){
        printf("FAIL: create_heap\n");
        return;
    }

    g_msg_bytes = msg_bytes;
    g_window = window;
    unsigned long long gc_calls = 0;

    pthread_t *t = (pthread_t*)calloc((size_t)nthreads, sizeof(pthread_t));
    WorkerArgs *args = (WorkerArgs*)calloc((size_t)nthreads, sizeof(WorkerArgs));
    if (!t || !args) {
        printf("FAIL: calloc failled (t=%p, args=%p)\n", (void*)t, (void*)args);
        free(t);
        free(args);
        destroy_heap(g_heap);   
        g_heap = NULL;
        return;
    }

    atomic_store(&g_stop, 0);

    for (int i = 0; i < nthreads; i++) {
        args[i].id = i;
        args[i].seed = (unsigned int)time(NULL) ^ (unsigned int)(i * 2654435761u);
        args[i].ops = 0ULL;
        pthread_create(&t[i], NULL, stress_worker, &args[i]);
    }

    for (int s = 1; s <= seconds; s++) {
        for (int k = 0; k < 10; k++) {
            collect_heap(g_heap);
            gc_calls++;
            usleep(100 * 1000); 
        }
        unsigned long long sum_ok = 0, sum_fail = 0, sum_frees = 0;
        for (int i = 0; i < nthreads; i++) {
            sum_ok += args[i].alloc_ok;
            sum_fail += args[i].alloc_fail;
            sum_frees += args[i].explicit_frees;
        }

        double mem = rss_mb();
        printf("[t=%2ds] gc_calls=%llu alloc_ok=%llu alloc_fail=%llu frees=%llu RAM_memory=%.1f MB\n", s, gc_calls, sum_ok, sum_fail, sum_frees, mem);   
    }

    atomic_store(&g_stop, 1);

    for (int i = 0; i < nthreads; i++)
        pthread_join(t[i], NULL);

    free(t);
    free(args);

    destroy_heap(g_heap);
    g_heap = NULL;

    printf("Test 3 completed.\n");
}

int main(void)
{
    printf("============TESTOVI ============\n");

    Heap *h = create_heap(1024 * 1024, 0);
    if(!h){return -1;}

    run_roots_gc_test(h);

    destroy_heap(h);

    run_segment_growth_test();

    int ths[] = { 1, 2, 5, 10 };
    for (int i = 0; i < 4; i++) {
        run_multithread_bigmem_test(ths[i], 15, 256 * 1024u, 4);
    }

    printf("==========================================================================\n");

    return 0;
}
