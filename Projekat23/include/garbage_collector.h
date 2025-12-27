#ifndef GARBAGE_COLLECTOR_H
#define GARBAGE_COLLECTOR_H

#include "pointers.h"
#include <stddef.h>

typedef struct GarbageCollector GarbageCollector;

GarbageCollector* gc_create(Pointers* ptrs);
void gc_destroy(GarbageCollector* gc);
void* gc_alloc(GarbageCollector* gc, size_t size);
void gc_collect(GarbageCollector* gc);

#endif