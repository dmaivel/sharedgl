#include <client/spinlock.h>
#include <emmintrin.h>

#ifdef _WIN32
#include <windows.h>
#endif

void spin_lock(int *lock)
{
#ifndef _WIN32
    while (!__sync_bool_compare_and_swap(lock, 0, 1))
        _mm_pause();
#else
    while (InterlockedCompareExchange(lock, 1, 0))
        _mm_pause();
#endif
}

void spin_unlock(int volatile *lock)
{
#ifndef _WIN32
    asm volatile ("":::"memory");
#endif
    *lock = 0;
}