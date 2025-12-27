#ifndef POINTERS_H
#define POINTERS_H

#include <stddef.h>

typedef struct Pointers {
    void** items; //pokazivaci 
    size_t count; //koliko elemenata ima u nizu
    size_t capacity; //koliko memorije je zauzeto za niz
} Pointers;

void pointers_init(Pointers* ptrs);  
int pointers_add(Pointers* ptrs, void* item);  
void pointers_free(Pointers* ptrs);
int pointers_remove(Pointers* ptrs, void* item);
typedef void (*PointersVisitor)(void* ptr, void* user_data);
void pointers_foreach(Pointers* p, PointersVisitor visitor, void* user_data);

#endif