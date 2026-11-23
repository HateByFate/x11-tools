/* Проверка: заканчивается ли перетаскивание на отпускании кнопки? */
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
    msleep(45);
}

static void origin(Window w, int *x, int *y)
{
    Window c;
    XTranslateCoordinates(dpy, w, root, 0, 0, x, y, &c);
}

int main(void)
{
    void *lib;
    Window w;
    XSetWindowAttributes a;
    XClassHint ch = { (char *)"kitty", (char *)"kitty" };
    int x0, y0, xa, xb, ya, yb, cx, cy, i;

    dpy = XOpenDisplay(NULL);
    root = DefaultRootWindow(dpy);
    lib = dlopen("libXtst.so.6", RTLD_NOW);
    fake_button = (fb_fn)dlsym(lib, "XTestFakeButtonEvent");
    fake_motion = (fm_fn)dlsym(lib, "XTestFakeMotionEvent");

    a.background_pixel = WhitePixel(dpy, DefaultScreen(dpy));
    w = XCreateWindow(dpy, root, 300, 300, 240, 140, 0,
                      CopyFromParent, InputOutput, CopyFromParent,
                      CWBackPixel, &a);
    XStoreName(dpy, w, "drag-end-test");
    XSetClassHint(dpy, w, &ch);
    XMapWindow(dpy, w);
    XSync(dpy, False);
    msleep(700);

    {
        XWindowAttributes wa;
        XGetWindowAttributes(dpy, w, &wa);
        origin(w, &x0, &y0);
        cx = x0 + wa.width / 2;
        cy = y0 + wa.height / 2;
    }
    move_to(cx, cy);
    msleep(200);

    fake_button(dpy, 2, True, 0);
    XSync(dpy, False);
    msleep(100);
    for (i = 1; i <= 10; i++) {
        move_to(cx + 8 * i, cy + 4 * i);
        msleep(20);
    }
    origin(w, &xa, &ya);
    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    msleep(400);
    origin(w, &xb, &yb);
    printf("после драга (до отпускания): %d,%d\n", xa - x0, ya - y0);
    printf("после отпускания:            %d,%d\n", xb - x0, yb - y0);

    /* теперь ведём указатель без нажатой кнопки */
    for (i = 1; i <= 10; i++) {
        move_to(cx + 80 + 12 * i, cy + 40 + 6 * i);
        msleep(30);
    }
    msleep(300);
    origin(w, &xb, &yb);
    printf("после движения без кнопки:   %d,%d  <-- если выросло, драг не закончился\n",
           xb - x0, yb - y0);

    /* ещё одно нажатие-отпускание без движения — не «залипает» ли */
    fake_button(dpy, 2, True, 0);
    XSync(dpy, False);
    msleep(60);
    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    msleep(300);
    origin(w, &xb, &yb);
    printf("после повторного клика:      %d,%d\n", xb - x0, yb - y0);

    XDestroyWindow(dpy, w);
    XSync(dpy, False);
    return 0;
}
