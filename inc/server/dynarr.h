#ifndef _DYNARR_H_
#define _DYNARR_H_

#include <stddef.h>
#include <stdbool.h>

typedef bool(*dynarr_match_fn)(void *elem, void *data);

void *dynarr_alloc(void **root, size_t next_offset, size_t size);
void dynarr_free_element(void **root, size_t next_offset, dynarr_match_fn matcher, void *data);
void dynarr_free(void **root, size_t next_offset);

#endif