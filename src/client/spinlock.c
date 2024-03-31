#include <client/spinlock.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
#include <emmintrin.h>
#else
// #warning Non-x86 platform detected, assuming ARM.
#define _mm_pause() asm volatile("yield");
#endif

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