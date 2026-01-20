#include "heap_state.h"
#include <stdlib.h>

int roots_add(Heap *h, void **slot)
{
    if (!h || !slot)
    {
        return -1;
    }

    pthread_mutex_lock(&h->lock);

    for (size_t i = 0; i < h->roots_count; i++)
    {
        if (h->roots[i] == slot)
        {
            pthread_mutex_unlock(&h->lock);
            return 0;
        }
    }

    if (h->roots_count == h->roots_capacity)
    {
        size_t new_capacity = (h->roots_capacity == 0) ? 16 : h->roots_capacity * 2;
        void ***new_roots = (void ***)realloc(h->roots, new_capacity * sizeof(void **));
        if (!new_roots)
        {
            pthread_mutex_unlock(&h->lock);
            return -1;
        }
        h->roots = new_roots;
        h->roots_capacity = new_capacity;
    }

    h->roots[h->roots_count++] = slot;
    pthread_mutex_unlock(&h->lock);

    return 0;
}

int roots_remove(Heap *h, void **slot)
{
    if (!h || !slot)
    {
        return -1;
    }

    pthread_mutex_lock(&h->lock);

    for (size_t i = 0; i < h->roots_count; i++)
    {
        if (h->roots[i] == slot)
        {
            h->roots[i] = h->roots[h->roots_count - 1];
            h->roots_count--;
            pthread_mutex_unlock(&h->lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&h->lock);
    return -1;
}
