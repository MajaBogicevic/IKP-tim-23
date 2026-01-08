#include "../heap/heap.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    Heap* h = create_heap(1024 * 1024, 0);

   void* a = alloc_heap(h, 64);
void* b = alloc_heap(h, 64);

memcpy(b, &a, sizeof(void*));

roots_add(h, &b);
collect_heap(h);

void* c = alloc_heap(h, 64);
printf("a=%p b=%p c=%p\n", a, b, c);


    destroy_heap(h);
    return 0;
}
