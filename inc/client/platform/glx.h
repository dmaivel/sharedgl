#ifndef _SGL_GLX_H_
#define _SGL_GLX_H_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

typedef struct __GLXcontextRec *GLXContext;
typedef XID GLXPixmap;
typedef XID GLXDrawable;
/* GLX 1.3 and later */
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef XID GLXFBConfigID;
typedef XID GLXContextID;
typedef XID GLXWindow;
typedef XID GLXPbuffer;

void glXSwapBuffers(Display* dpy, GLXDrawable drawable);
void *glXGetProcAddress(char *s);
void* glXGetProcAddressARB(char* s);

#endif