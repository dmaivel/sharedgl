#include <client/spinlock.h>

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
    while (!__sync_bool_compare_and_swap(lock, 0, 1));
}

void spin_unlock(int volatile *lock)
{
    asm volatile ("":::"memory");
    *lock = 0;
}