#include "heap_state.h"
#include <stdlib.h>


int thread_register(Heap *h)
{
    if (!h)
        return -1;

    ThreadInfo *ti = calloc(1, sizeof(ThreadInfo));
    if (!ti)
        return -1;

    ti->tid = pthread_self();
    ti->status = THREAD_RUNNING;


    void *stack_hi = pthread_get_stackaddr_np(ti->tid);
    size_t stack_size = pthread_get_stacksize_np(ti->tid);
    ti->stack_hi = stack_hi;
    ti->stack_lo = (char *)stack_hi - stack_size;

    pthread_mutex_lock(&h->lock);
    ti->next = h->threads;
    h->threads = ti;
    pthread_mutex_unlock(&h->lock);

    return 0;
}

int thread_unregister(Heap *h)
{
    if (!h)
        return -1;

    pthread_t self = pthread_self();

    pthread_mutex_lock(&h->lock);
    ThreadInfo **pp = &h->threads;
    while (*pp)
    {
        if (pthread_equal((*pp)->tid, self))
        {
            ThreadInfo *dead = *pp;
            *pp = dead->next;
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&h->lock);
    return 0;
}

void gc_safepoint(Heap *h)
{
    if (!h)
        return;

    pthread_mutex_lock(&h->lock);

    if (!h->gc_requested)
    {
        pthread_mutex_unlock(&h->lock);
        return;
    }

    pthread_t self = pthread_self();
    ThreadInfo *ti = h->threads;
    while (ti)
    {
        if (pthread_equal(ti->tid, self))
        {
            ti->status = THREAD_PARKED;
            ti->sp = __builtin_frame_address(0);
            break;
        }
        ti = ti->next;
    }

    pthread_cond_broadcast(&h->gc_cond);

    while (h->gc_requested)
    {
        pthread_cond_wait(&h->gc_cond, &h->lock);
    }

    if (ti)
        ti->status = THREAD_RUNNING;

    pthread_mutex_unlock(&h->lock);
}
