#include "../heap/heap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

int main(void)
{

    Heap *h = create_heap(1024 * 1024, 0);
    assert(h != NULL);
    printf("[OK] create_heap\n");

    printf("\n[CASE 1] roots + transitive mark\n");

    void *a = alloc_heap(h, 64);
    assert(a != NULL);
    memset(a, 0xAB, 64);

    void *b = alloc_heap(h, sizeof(void *));
    assert(b != NULL);
    memcpy(b, &a, sizeof(void *)); 

    assert(roots_add(h, (void**)&b) == 0);
    printf("[OK] roots_add\n");

    collect_heap(h);
    printf("[OK] collect_heap \n");

    unsigned char *ua = (unsigned char*)a;
    for (int i = 0; i < 64; i++)
        assert(ua[i] == 0xAB);
    printf("[OK] a content preserved after GC\n");

    printf("\n[CASE 2] roots_remove + sweep + reuse\n");

    assert(roots_remove(h, (void**)&b) == 0);
    printf("[OK] roots_remove\n");

    uintptr_t addr_a = (uintptr_t)a;

    a = NULL;
    b = NULL;

    collect_heap(h);
    printf("[OK] collect_heap\n");

    void *x = alloc_heap(h, 64);
    assert(x != NULL);

    if ((uintptr_t)x == addr_a)
        printf("[OK] reuse: new block x is on the same address as a\n", x);
    else
        printf("[INFO] reuse not the same address\n", x);

    free_heap(h, x);
    printf("[OK] free_heap(x)\n");


    printf("\n[CASE 3] free_heap reuse sanity\n");

    void *p = alloc_heap(h, 128);
    assert(p != NULL);
    uintptr_t addr_p = (uintptr_t)p;

    free_heap(h, p);

    void *q = alloc_heap(h, 128);
    assert(q != NULL);

    if ((uintptr_t)q == addr_p)
        printf("[OK] free reuse: q same address as p\n", q);
    else
        printf("[INFO] free reuse not the same address, but allocator can return another block\n", q);

    free_heap(h, q);
    printf("[OK] free_heap(q)\n");


    printf("\n[CASE 4] thread_register/thread_unregister in main thread\n");

    assert(thread_register(h) == 0);
    printf("[OK] thread_register\n");

    void *t = alloc_heap(h, 32);
    assert(t != NULL);

    collect_heap(h);
    printf("[OK] collect_heap while registered\n");

    free_heap(h, t);

    assert(thread_unregister(h) == 0);
    printf("[OK] thread_unregister\n");

    destroy_heap(h);
    printf("\n[OK] destroy_heap\n");

    

    printf("\nALL TESTS: PASS\n");

    return 0;
}
