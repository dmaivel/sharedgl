#ifndef _ICD_H_
#define _ICD_H_

#define ICD_SET_MAX_DIMENSIONS_DEFINITION(mw, mh, rw, rh) \
    void icd_set_max_dimensions(int width, int height) \
    { \
        mw = width; \
        mh = height; \
        rw = 16; \
        rh = 16; \
    }

#define ICD_RESIZE_DEFINITION(rw, rh) \
    void icd_resize(int width, int height) \
    { \
        rw = width; \
        rh = height; \
    } \

void icd_set_max_dimensions(int width, int height);
void icd_resize(int width, int height);

#endif