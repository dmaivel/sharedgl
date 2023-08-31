#include <server/dynarr.h>
#include <stdlib.h>

void *dynarr_alloc(void **root, size_t next_offset, size_t size)
{
    /*
     * if list hasn't started, then start the dam list
     */
    if (*root == NULL) {
        *root = calloc(1, size);
        return *root;
    }

    /*
     * otherwise, iterate through the list until we find the next spot to allocate to
     */
    void *next;

    /*
     * iterate through root until next = NULL
     */
    for (next = *root; *(void**)(next + next_offset); next = *(void**)(next + next_offset));

    /* 
     * allocation 
     */
    *(void**)(next + next_offset) = calloc(1, size);

    return *(void**)(next + next_offset);
}

void dynarr_free_element(void **root, size_t next_offset, dynarr_match_fn matcher, void *data)
{
    void *prev = NULL;

    for (void *elem = *root; elem;) {
        void *next = *(void**)(elem + next_offset);

        if (matcher(elem, data)) {
            /*
             * check if element is first, otherwise doesn't matter
             */
            if (prev == NULL)
                *root = next;
            else
                *(void**)(prev + next_offset) = next;

            free(elem);
        }
        else
            prev = elem;

        elem = next;
    }
}

void dynarr_free(void **root, size_t next_offset)
{
    /* 
     * recursively call up until the last element 
     * (elements need to be freed in reverse order) 
     */
    if (*(void**)(root)) {
        dynarr_free(*(void**)(root + next_offset), next_offset);
        free(*root);
    }
}