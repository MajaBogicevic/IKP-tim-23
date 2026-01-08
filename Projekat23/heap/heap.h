#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Heap Heap;

Heap* create_heap(size_t segment_size_bytes, size_t gc_threshold_bytes);
void  destroy_heap(Heap* h);

void* alloc_heap(Heap* h, size_t size_bytes);
void  free_heap(Heap* h, void* ptr);

void  collect_heap(Heap* h);

int   roots_add(Heap* h, void** slot);
int   roots_remove(Heap* h, void** slot);

int   thread_register(Heap* h);
int   thread_unregister(Heap* h);
void  gc_safepoint(Heap* h);

#ifdef __cplusplus
}
#endif

#endif 
