#ifndef HEAP_INTERNAL_H
#define HEAP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGNMENT ((size_t)sizeof(void*))

static inline size_t heap_align_up(size_t x)
{
    size_t a = HEAP_ALIGNMENT;
    return (x + (a - 1)) & ~(a - 1);
}

#define BLOCK_MAGIC 0xC0FFEE01u


#define BLOCK_FLAG_FREE  (1u << 0)
#define BLOCK_FLAG_MARK  (1u << 1)

typedef struct BlockHeader BlockHeader;
struct BlockHeader {
    size_t      size;      
    BlockHeader* next_free;
    uint32_t    magic;
    uint32_t    flags;
};

typedef struct Segment Segment;
struct Segment {
    unsigned char* mem;
    size_t size;
    Segment* next;
};

#endif 
