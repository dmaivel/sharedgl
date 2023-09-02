#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

void spin_set(int *lock);
int *spin_get();

void spin_lock(int *lock);
void spin_unlock(int volatile *lock);

#endif