#include "../heap/heap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

static Heap *g_heap = NULL;
static atomic_int g_stop = 0;

static size_t g_msg_bytes = 256 * 1024; 
static int g_window = 16;

static unsigned long long *g_ops = NULL;

static void *worker(void *arg)
{
    int id = (int)(long)arg;

    thread_register(g_heap);

    void *win[64];
    int W = g_window;
    if (W > 64) W = 64;
    for (int i = 0; i < W; i++) win[i] = NULL;

    void * volatile keep = alloc_heap(g_heap, g_msg_bytes);
    if (keep) memset((void*)keep, (unsigned char)(0xA0 + (id & 0x0F)), 128);

    unsigned long long cnt = 0;
    unsigned long long idx = 0;

    while (!atomic_load(&g_stop))
    {
        void *p = alloc_heap(g_heap, g_msg_bytes);
        if (p)
        {
            memset(p, (unsigned char)(id + 1), 64);
            win[idx % (unsigned long long)W] = p;
            idx++;

            if ((cnt % 256ULL) == 0ULL)
            {
                for (int j = 0; j < W / 2; j++)
                    win[(idx + (unsigned long long)j) % (unsigned long long)W] = NULL;
            }

            // povremeno free jednog bloka da stresira i free-list
            if ((cnt % 512ULL) == 0ULL)
            {
                int k = (int)((idx + 3ULL) % (unsigned long long)W);
                void *f = win[k];
                win[k] = NULL;
                if (f) free_heap(g_heap, f);
            }
        }

        cnt++;
    }

    // cleanup
    for (int i = 0; i < W; i++) win[i] = NULL;
    keep = NULL;

    thread_unregister(g_heap);

    g_ops[id] = cnt;
    return NULL;
}

int run_test(int nthreads, int seconds, size_t msg_bytes, int window)
{
    if (nthreads <= 0) nthreads = 1;
    if (seconds <= 0) seconds = 5;
    if (msg_bytes < 1024) msg_bytes = 1024;
    if (window <= 0) window = 8;

    g_msg_bytes = msg_bytes;
    g_window = window;

    printf("\n=== BIG MEM + MULTI THREAD TEST ===\n");
    printf("threads=%d, seconds=%d, msg=%zu KB, window=%d\n",
           nthreads, seconds, g_msg_bytes / 1024, g_window);

    g_heap = create_heap(1024 * 1024, 0);
    if (!g_heap) {
        printf("[FAIL] create_heap\n");
        return 1;
    }
    printf("[OK] create_heap\n");

    for (int r = 0; r < 2; r++) {
        void *big[8] = {0};
        for (int i = 0; i < 8; i++) {
            big[i] = alloc_heap(g_heap, 1024 * 1024);
            if (big[i]) memset(big[i], 0xCD, 256);
        }
        for (int i = 0; i < 8; i++) big[i] = NULL;
        collect_heap(g_heap);
    }
    printf("[OK] warmup\n");

    pthread_t *t = (pthread_t*)calloc((size_t)nthreads, sizeof(pthread_t));
    g_ops = (unsigned long long*)calloc((size_t)nthreads, sizeof(unsigned long long));
    if (!t || !g_ops) {
        printf("[FAIL] calloc\n");
        free(t);
        free(g_ops);
        destroy_heap(g_heap);
        return 1;
    }

    atomic_store(&g_stop, 0);

    for (int i = 0; i < nthreads; i++)
        pthread_create(&t[i], NULL, worker, (void*)(long)i);

    for (int s = 0; s < seconds; s++) {
        for (int k = 0; k < 10; k++) {
            collect_heap(g_heap);
            usleep(100 * 1000); 
        }
        printf("... %d/%d sec\n", s + 1, seconds);
    }

    atomic_store(&g_stop, 1);

    for (int i = 0; i < nthreads; i++)
        pthread_join(t[i], NULL);

    unsigned long long total = 0;
    for (int i = 0; i < nthreads; i++) total += g_ops[i];

    printf("[RESULT] total ops = %llu\n", total);

    free(t);
    free(g_ops);

    destroy_heap(g_heap);
    printf("[OK] destroy_heap\n");
    return 0;
}

#ifdef STANDALONE_TEST
int main(int argc, char **argv)
{
    int nthreads = 10;
    int seconds  = 15;
    size_t kb    = 256;
    int window   = 16;

    if (argc >= 2) nthreads = atoi(argv[1]);
    if (argc >= 3) seconds  = atoi(argv[2]);
    if (argc >= 4) kb       = (size_t)atoi(argv[3]);
    if (argc >= 5) window   = atoi(argv[4]);

    return run_test(nthreads, seconds, kb * 1024u, window);
}
#endif
