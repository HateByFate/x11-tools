/*
 * midmove.c — перетаскивание окна kitty средней кнопкой мыши (X11 + GNOME/mutter).
 *
 * Зачем: у kitty в конфиге hide_window_decorations yes, поэтому окно без заголовка
 * и двигать его можно только Alt+drag. Средняя кнопка в GNOME/mutter для
 * перемещения окон не настраивается, поэтому нужен этот помощник.
 *
 * Как работает:
 *   1. Пассивный захват Button2 на корневом окне (только «голая» средняя кнопка,
 *      с учётом CapsLock/NumLock; Shift/Ctrl/Alt/Super не перехватываются).
 *   2. Нажимаем среднюю в окне kitty (WM_CLASS=kitty) — нажатие остаётся у нас,
 *      указатель расмораживается, ждём движения.
 *   3. Сдвинули указатель больше порога — отдаём перетаскивание mutter через
 *      EWMH _NET_WM_MOVERESIZE (action=MOVE, button=2): окно едет как при
 *      перетаскивании за заголовок (плавно, с прилипанием к краям).
 *   4. mutter по своей воле завершает такой drag только по отпусканию 1-й или
 *      3-й кнопки (см. process_pointer_event в meta-window-drag.c), поэтому
 *      отпускание средней отслеживаем сами и посылаем _NET_WM_MOVERESIZE_CANCEL.
 *   5. Если движения не было — переиздаём клик через XTest: вставка средней
 *      кнопкой в kitty работает как раньше.
 *   6. В чужих окнах (не kitty) нажатие сразу отдаётся приложению через
 *      ReplayPointer — там средняя кнопка работает полностью штатно.
 *
 * Сборка:  gcc -O2 -Wall -Wextra -o midmove midmove.c -lX11 -ldl
 * Запуск:  ./midmove [-v] [-t порог_в_пикселях]
 */

#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define DEFAULT_THRESHOLD 3
#define MAX_TREE_DEPTH    16
#define IDLE              0
#define PRESSED           1
#define DRAGGING          2

static Display *dpy;
static Window   root;
static Atom     a_moveresize;
static Atom     a_wm_state;
static Atom     a_wm_class;
static int      verbose;
static int      grab_failed;

/* XTest подключаем через dlopen: в Debian-пакете libxtst6 нет заголовков,
 * а тащить libxtst-dev ради одной функции не хочется. */
typedef Bool (*fake_button_fn)(Display *, unsigned int, Bool, unsigned long);
static fake_button_fn fake_button;

static const unsigned int grab_mods[] = {
    0, LockMask, Mod2Mask, LockMask | Mod2Mask
};
#define NGRABS (sizeof grab_mods / sizeof grab_mods[0])

static int ignore_x_errors(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess)
        grab_failed = 1; /* кнопку уже захватил другой экземпляр */
    return 0; /* окна в процессе перетаскивания могут исчезать — не падаем */
}

static int has_property(Window w, Atom prop)
{
    Atom type = None;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    int status;

    status = XGetWindowProperty(dpy, w, prop, 0, 0, False, AnyPropertyType,
                                &type, &format, &n, &after, &data);
    if (data)
        XFree(data);
    return status == Success && type != None;
}

/* Аналог XmuClientWindow(): ищем клиентское окно внутри рамки WM. */
static Window find_client(Window w, Atom marker, int depth)
{
    Window rroot, parent, *children = NULL;
    unsigned int nchildren = 0;
    Window found = None;

    if (w == None || depth > MAX_TREE_DEPTH)
        return None;

    if (has_property(w, marker))
        return w;

    if (!XQueryTree(dpy, w, &rroot, &parent, &children, &nchildren))
        return None;

    for (unsigned int i = 0; i < nchildren && found == None; i++)
        found = find_client(children[i], marker, depth + 1);

    if (children)
        XFree(children);
    return found;
}

static Window pick_target(Window under_pointer)
{
    Window w;

    if (under_pointer == None)
        return None;

    w = find_client(under_pointer, a_wm_state, 0);
    if (w == None)
        w = find_client(under_pointer, a_wm_class, 0);
    return w;
}

/* Нас интересуют только окна kitty: в остальных ведём себя как обычная мышь. */
static int is_kitty(Window w)
{
    XClassHint ch = { NULL, NULL };
    int ok = 0;

    if (w == None || !XGetClassHint(dpy, w, &ch))
        return 0;

    if (ch.res_name && strcasecmp(ch.res_name, "kitty") == 0)
        ok = 1;
    if (ch.res_class && strcasecmp(ch.res_class, "kitty") == 0)
        ok = 1;

    if (ch.res_name)
        XFree(ch.res_name);
    if (ch.res_class)
        XFree(ch.res_class);
    return ok;
}

static void grab_passive(void)
{
    for (size_t i = 0; i < NGRABS; i++)
        XGrabButton(dpy, Button2, grab_mods[i], root, False,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeSync, GrabModeAsync, None, None);
    XFlush(dpy);
}

static void ungrab_passive(void)
{
    for (size_t i = 0; i < NGRABS; i++)
        XUngrabButton(dpy, Button2, grab_mods[i], root);
    XFlush(dpy);
}

/* Чужое окно: отдаём нажатие приложению средствами сервера, оно получает
 * обычный клик в момент нажатия (вставка, средний драг и т.п.). */
static void passthrough_press(void)
{
    XAllowEvents(dpy, ReplayPointer, CurrentTime);
    XFlush(dpy);
}

/* Переиздаём клик средней кнопкой, чтобы kitty получила вставку. */
static void synthesize_click(void)
{
    if (!fake_button) {
        fprintf(stderr, "midmove: libXtst недоступна, клик средней кнопкой потерян\n");
        return;
    }

    ungrab_passive();          /* иначе собственный захват съест это событие */
    fake_button(dpy, 2, True, 0);
    fake_button(dpy, 2, False, 0);
    XSync(dpy, False);
    grab_passive();
}

/* Отдаём перетаскивание оконному менеджеру. */
static void send_moveresize(Window target, int x_root, int y_root)
{
    XEvent ev;

    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = target;
    ev.xclient.message_type = a_moveresize;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = x_root;
    ev.xclient.data.l[1] = y_root;
    ev.xclient.data.l[2] = 8;  /* _NET_WM_MOVERESIZE_MOVE */
    ev.xclient.data.l[3] = 2;  /* button */
    ev.xclient.data.l[4] = 1;  /* source indication: приложение */

    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

/* Завершаем перетаскивание, начатое через _NET_WM_MOVERESIZE. */
static void send_moveresize_cancel(Window target)
{
    XEvent ev;

    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = target;
    ev.xclient.message_type = a_moveresize;
    ev.xclient.format = 32;
    ev.xclient.data.l[2] = 11; /* _NET_WM_MOVERESIZE_CANCEL */

    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

/* Ждём событие не дольше ms миллисекунд. Возвращает 1, если событие есть. */
static int wait_event(XEvent *ev, int ms)
{
    int fd;
    fd_set fds;
    struct timeval tv;

    if (XPending(dpy)) {
        XNextEvent(dpy, ev);
        return 1;
    }

    XFlush(dpy);
    fd = ConnectionNumber(dpy);
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
        XNextEvent(dpy, ev);
        return 1;
    }
    return 0;
}

static int button2_still_down(void)
{
    Window rret, cret;
    int wx, wy, rx, ry;
    unsigned int mask = 0;

    if (!XQueryPointer(dpy, root, &rret, &cret, &rx, &ry, &wx, &wy, &mask))
        return 0;
    return (mask & Button2Mask) != 0;
}

int main(int argc, char **argv)
{
    int threshold = DEFAULT_THRESHOLD;
    int state = IDLE;
    int x0 = 0, y0 = 0;
    Window target = None;
    void *lib;
    int opt;

    while ((opt = getopt(argc, argv, "vt:h")) != -1) {
        switch (opt) {
        case 'v': verbose = 1; break;
        case 't': threshold = atoi(optarg); break;
        default:
            fprintf(stderr,
                    "usage: %s [-v] [-t threshold_px]\n"
                    "  Средняя кнопка в окне kitty: тащить окно; без движения — обычный клик.\n",
                    argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (threshold < 1)
        threshold = 1;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "midmove: не удалось открыть X-дисплей (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "?");
        return 1;
    }

    lib = dlopen("libXtst.so.6", RTLD_NOW);
    if (lib)
        *(void **)&fake_button = dlsym(lib, "XTestFakeButtonEvent");

    XSetErrorHandler(ignore_x_errors);
    root = DefaultRootWindow(dpy);
    a_moveresize = XInternAtom(dpy, "_NET_WM_MOVERESIZE", False);
    a_wm_state   = XInternAtom(dpy, "WM_STATE", False);
    a_wm_class   = XInternAtom(dpy, "WM_CLASS", False);

    grab_passive();
    XSync(dpy, False);
    if (grab_failed) {
        fprintf(stderr, "midmove: уже запущен (кнопка 2 уже захвачена), выходим\n");
        return 0;
    }
    if (verbose)
        fprintf(stderr, "midmove: запущен (порог %d px, XTest %s)\n",
                threshold, fake_button ? "есть" : "НЕТ");

    for (;;) {
        XEvent ev;

        if (state == DRAGGING) {
            /* Перетаскиванием владеет mutter — событий нам не шлёт, поэтому
             * отпускание кнопки отслеживаем опросом. */
            if (!wait_event(&ev, 8)) {
                if (!button2_still_down()) {
                    send_moveresize_cancel(target);
                    if (verbose)
                        fprintf(stderr, "midmove: отпущено — перетаскивание завершено\n");
                    state = IDLE;
                }
                continue;
            }
        } else {
            XNextEvent(dpy, &ev);
        }

        switch (ev.type) {
        case ButtonPress:
            if (ev.xbutton.button != Button2 || ev.xbutton.send_event ||
                state != IDLE)
                break;

            x0 = ev.xbutton.x_root;
            y0 = ev.xbutton.y_root;
            target = pick_target(ev.xbutton.subwindow);

            if (!is_kitty(target)) {
                if (verbose)
                    fprintf(stderr, "midmove: чужое окно 0x%lx — обычный клик\n",
                            (unsigned long)target);
                passthrough_press();
                break;
            }

            state = PRESSED;
            /* Нажатие осталось у нас: расмораживаем указатель, чтобы получать
             * движение и отпускание. */
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
            XFlush(dpy);
            if (verbose)
                fprintf(stderr, "midmove: kitty 0x%lx, нажатие %d,%d\n",
                        (unsigned long)target, x0, y0);
            break;

        case MotionNotify:
            if (state != PRESSED)
                break;
            if (abs(ev.xmotion.x_root - x0) < threshold &&
                abs(ev.xmotion.y_root - y0) < threshold)
                break;

            state = DRAGGING;
            XUngrabPointer(dpy, CurrentTime);
            XFlush(dpy);
            send_moveresize(target, x0, y0);
            if (verbose)
                fprintf(stderr, "midmove: тащим окно 0x%lx\n",
                        (unsigned long)target);
            break;

        case ButtonRelease:
            if (state != PRESSED || ev.xbutton.button != Button2)
                break;
            state = IDLE;
            XUngrabPointer(dpy, CurrentTime);
            XFlush(dpy);
            synthesize_click();
            if (verbose)
                fprintf(stderr, "midmove: отпущено без движения — переиздаю клик\n");
            break;

        default:
            break;
        }
    }
}
