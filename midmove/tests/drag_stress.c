/* Долгое перетаскивание окна средней кнопкой: для замера нагрузки. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef Bool (*fb_fn)(Display *, unsigned int, Bool, unsigned long);
typedef Bool (*fm_fn)(Display *, int, int, int, unsigned long);

static Display *dpy;
static Window root;
static fb_fn fake_button;
static fm_fn fake_motion;

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    XSync(dpy, False);
}

static void move_to(int x, int y)
{
    fake_motion(dpy, DefaultScreen(dpy), x, y, 0);
    XSync(dpy, False);
}

int main(int argc, char **argv)
{
    void *lib;
    Window w;
    XSetWindowAttributes a;
    XClassHint ch = { (char *)"kitty", (char *)"kitty" };
    int cx, cy, i, steps = 600;

    if (argc > 1)
        steps = atoi(argv[1]);

    dpy = XOpenDisplay(NULL);
    root = DefaultRootWindow(dpy);
    lib = dlopen("libXtst.so.6", RTLD_NOW);
    fake_button = (fb_fn)dlsym(lib, "XTestFakeButtonEvent");
    fake_motion = (fm_fn)dlsym(lib, "XTestFakeMotionEvent");

    a.background_pixel = WhitePixel(dpy, DefaultScreen(dpy));
    w = XCreateWindow(dpy, root, 300, 300, 240, 140, 0,
                      CopyFromParent, InputOutput, CopyFromParent,
                      CWBackPixel, &a);
    XStoreName(dpy, w, "drag-stress");
    XSetClassHint(dpy, w, &ch);
    XMapWindow(dpy, w);
    XSync(dpy, False);
    msleep(700);

    {
        XWindowAttributes wa;
        int x, y;
        Window c;
        XGetWindowAttributes(dpy, w, &wa);
        XTranslateCoordinates(dpy, w, root, 0, 0, &x, &y, &c);
        cx = x + wa.width / 2;
        cy = y + wa.height / 2;
    }

    move_to(cx, cy);
    msleep(200);
    fake_button(dpy, 2, True, 0);
    XSync(dpy, False);
    msleep(80);

    /* 10 мс на шаг — примерно как при обычном ведении мыши */
    for (i = 0; i < steps; i++) {
        move_to(cx + (i % 200) - 100, cy + ((i * 7) % 200) - 100);
        msleep(10);
    }

    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    msleep(300);
    XDestroyWindow(dpy, w);
    XSync(dpy, False);
    printf("перетаскивание: %d шагов\n", steps);
    return 0;
}
