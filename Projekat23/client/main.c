#include "../heap/heap.h"
#include <stdio.h>

int main(void)
{
    Heap* h = create_heap(1024 * 1024, 8 * 1024 * 1024);
    if (!h) {
        printf("heap_create failed\n");
        return 1;
    }

    void* p = alloc_heap(h, 24);
    roots_add(h, &p);
    void* q = alloc_heap(h, 100);
    printf("p=%p \n", p);
    printf("q=%p \n", q);

    roots_remove(h, &p);
    free_heap(h, p);
    free_heap(h, q);

    void* r = alloc_heap(h, 50);
    printf("r=%p\n", r);

    destroy_heap(h);
    return 0;
}
