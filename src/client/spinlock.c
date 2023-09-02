#include <client/spinlock.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int *keep_this_lock_pls;

void spin_set(int *lock)
{
    keep_this_lock_pls = lock;
}

int *spin_get()
{
    return keep_this_lock_pls;
}

void spin_lock(int *lock)
{
#ifndef _WIN32
    while (!__sync_bool_compare_and_swap(lock, 0, 1));
#else
    while (!InterlockedCompareExchange(lock, 1, 0));
#endif
}

void spin_unlock(int volatile *lock)
{
#ifndef _WIN32
    asm volatile ("":::"memory");
#endif
    *lock = 0;
}