/* Тест midmove (XTest через dlopen): только окна kitty таскаются средней кнопкой,
 * в чужих окнах клик должен приходить сразу, как обычно. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef Bool (*fake_button_fn)(Display *, unsigned int, Bool, unsigned long);
typedef Bool (*fake_motion_fn)(Display *, int, int, int, unsigned long);

static Display *dpy;
static Window root;
static fake_button_fn fake_button;
static fake_motion_fn fake_motion;

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

static Window make_window(const char *name, const char *res_name,
                          const char *res_class, long event_mask)
{
    XSetWindowAttributes a;
    XClassHint ch = { (char *)res_name, (char *)res_class };
    Window w;

    a.background_pixel = WhitePixel(dpy, DefaultScreen(dpy));
    a.event_mask = event_mask;
    w = XCreateWindow(dpy, root, 300, 300, 240, 140, 0,
                      CopyFromParent, InputOutput, CopyFromParent,
                      CWBackPixel | CWEventMask, &a);
    XStoreName(dpy, w, name);
    XSetClassHint(dpy, w, &ch);
    XMapWindow(dpy, w);
    XSync(dpy, False);
    msleep(700);
    return w;
}

static void origin(Window w, int *x, int *y)
{
    Window child;
    XTranslateCoordinates(dpy, w, root, 0, 0, x, y, &child);
}

static void center(Window w, int *cx, int *cy)
{
    XWindowAttributes wa;
    int x, y;
    XGetWindowAttributes(dpy, w, &wa);
    origin(w, &x, &y);
    *cx = x + wa.width / 2;
    *cy = y + wa.height / 2;
}

static int pending_button(int button, int type)
{
    int n = 0;
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == type && ev.xbutton.button == (unsigned int)button)
            n++;
    }
    return n;
}

int main(void)
{
    void *lib;
    Window w;
    int x0, y0, x1, y1, cx, cy, i, ok;
    long score = 0;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 2; }
    root = DefaultRootWindow(dpy);

    lib = dlopen("libXtst.so.6", RTLD_NOW);
    if (!lib) { fprintf(stderr, "libXtst.so.6: %s\n", dlerror()); return 2; }
    fake_button = (fake_button_fn)dlsym(lib, "XTestFakeButtonEvent");
    fake_motion = (fake_motion_fn)dlsym(lib, "XTestFakeMotionEvent");
    if (!fake_button || !fake_motion) { fprintf(stderr, "dlsym failed\n"); return 2; }

    /* ---- тест 1: окно kitty тащится средней кнопкой ---- */
    w = make_window("midmove-drag-test", "kitty", "kitty", 0);
    origin(w, &x0, &y0);
    center(w, &cx, &cy);
    move_to(cx, cy);
    msleep(200);

    fake_button(dpy, 2, True, 0);
    XSync(dpy, False);
    msleep(120);
    for (i = 1; i <= 12; i++) {
        move_to(cx + 8 * i, cy + 4 * i);
        msleep(20);
    }
    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    msleep(700);

    origin(w, &x1, &y1);
    ok = abs((x1 - x0) - 96) <= 30 && abs((y1 - y0) - 48) <= 30;
    printf("[%s] kitty: перетаскивание средней кнопкой, смещение %d,%d (ждали ~96,48)\n",
           ok ? "OK " : "FAIL", x1 - x0, y1 - y0);
    score += ok;
    XDestroyWindow(dpy, w);
    XSync(dpy, False);
    msleep(250);

    /* ---- тест 2: клик в окно kitty доходит (вставка) ---- */
    w = make_window("midmove-click-test", "kitty", "kitty",
                    ButtonPressMask | ButtonReleaseMask);
    center(w, &cx, &cy);
    move_to(cx, cy);
    msleep(200);
    fake_button(dpy, 2, True, 0);
    XSync(dpy, False);
    msleep(20);
    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    msleep(500);

    ok = pending_button(2, ButtonPress) >= 1;
    printf("[%s] kitty: клик средней кнопкой доходит до окна (вставка)\n",
           ok ? "OK " : "FAIL");
    score += ok;
    XDestroyWindow(dpy, w);
    XSync(dpy, False);
    msleep(250);

    /* ---- тест 3: чужое окно получает нажатие сразу, драга нет ---- */
    w = make_window("midmove-foreign-test", "midmovetest", "MidmoveTest",
                    ButtonPressMask | ButtonReleaseMask);
    origin(w, &x0, &y0);
    center(w, &cx, &cy);
    move_to(cx, cy);
    msleep(200);

    fake_button(dpy, 2, True, 0);
    XSync(dpy, False);
    msleep(150);           /* ещё держим кнопку */
    ok = pending_button(2, ButtonPress) >= 1;
    printf("[%s] чужое окно: нажатие доставлено сразу, без ожидания отпускания\n",
           ok ? "OK " : "FAIL");
    for (i = 1; i <= 8; i++) {
        move_to(cx + 6 * i, cy + 3 * i);
        msleep(20);
    }
    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    msleep(500);
    origin(w, &x1, &y1);
    ok = ok && (x1 == x0 && y1 == y0) && pending_button(2, ButtonRelease) >= 1;
    printf("[%s] чужое окно: окно не сдвинулось (%d,%d), отпускание дошло\n",
           ok ? "OK " : "FAIL", x1 - x0, y1 - y0);
    score += ok;
    XDestroyWindow(dpy, w);
    XSync(dpy, False);

    printf("\nитог: %ld/3\n", score);
    return score == 3 ? 0 : 1;
}
