/*
 * headbat.c — заряд наушников Logitech G435 в трее GNOME + уведомления.
 *
 * Зачем: G HUB не поддерживает G435, а стандартный UPower заряд этого донгла
 * (046d:0acb) не отдаёт. Заряд читается по реверс-инженеренному HID-протоколу.
 *
 * Как работает:
 *   1. Находит hidraw-узел донгла 046d:0acb через sysfs (HID_ID).
 *   2. Раз в interval секунд читает заряд: входит в «bootloader» моста Realtek,
 *      спрашивает статус подключения и напряжение аккумулятора, выходит.
 *      ВАЖНО: на время чтения прошивка глушит звук (~0.6 с), поэтому по
 *      умолчанию опрос редкий и пропускается, пока наушники играют звук.
 *   3. Показывает процент в трее (Ayatana AppIndicator) и шлёт уведомления
 *      при низком заряде / подключении / отключении.
 *
 * Сборка:  см. Makefile (нужны заголовки gtk+-3.0, ayatana-appindicator3-0.1,
 *          libnotify; рантайм-библиотеки в Debian стоят по умолчанию).
 * Запуск:  ./headbat [--interval N] [--status] [--no-tray] [-v]
 *
 * Протокол: deddroid/logitech-g435-battery (MIT) + реверс endes. См. README.md.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

#include "headbat.h"

/* ---- параметры по умолчанию ---- */
#define DEFAULT_INTERVAL 900    /* секунд между опросами (15 минут, как в g435-tray) */
#define DEFAULT_MAX_SILENCE_WAIT 1800 /* ждём тишины не дольше 30 мин */
#define LOW_WARN         20     /* % — первое предупреждение */
#define LOW_CRIT         10     /* % — критическое предупреждение */
#define VID_LOGITECH     0x046d
#define PID_G435         0x0acb

static int verbose;
static int opt_interval = DEFAULT_INTERVAL;
static int opt_status;          /* разовый вывод и выход */
static int opt_no_tray;
static int opt_always;          /* не пропускать опрос при звуке */
static int opt_max_silence_wait = DEFAULT_MAX_SILENCE_WAIT; /* макс. ожидание тишины */

static void dbg(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[headbat] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* ================= поиск hidraw-узла донгла ================= */

/* Ищем /sys/class/hidraw/hidrawN, у которого HID_ID = 0003:0000046D:00000ACB. */
static int find_dongle(char *path, size_t pathlen)
{
    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;
        char uevent[512];
        snprintf(uevent, sizeof uevent, "/sys/class/hidraw/%s/device/uevent",
                 e->d_name);
        FILE *f = fopen(uevent, "r");
        if (!f)
            continue;
        char line[256];
        unsigned bus, vid, pid;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3 &&
                vid == VID_LOGITECH && pid == PID_G435) {
                snprintf(path, pathlen, "/dev/%s", e->d_name);
                found = 0;
                break;
            }
        }
        fclose(f);
        if (found == 0)
            break;
    }
    closedir(d);
    return found;
}

/* ================= HID-транспорт (донгл 0acb) ================= */

#define REPORT_ID     6
#define FEATURE_LEN   64
#define OUTPUT_LEN    515
#define ACK_OK        0x5a
#define ACK_ALREADY   0xfe

static const uint8_t OP_ENTER[2] = { 0x01, 0x03 };
static const uint8_t OP_EXIT[2]  = { 0x08, 0x03 };
static const uint8_t OP_SEND[2]  = { 0x04, 0x03 };

enum {
    HB_OK = 0,
    HB_ERR_NODEV,      /* донгл не найден */
    HB_ERR_OPEN,       /* нет доступа к /dev/hidraw* (нужен udev-rule) */
    HB_ERR_OFF,        /* наушники выключены / не подключены */
    HB_ERR_NOREPLY,    /* донгл не ответил */
    HB_ERR_IO,         /* ошибка ввода-вывода */
};

static int g_fd = -1;

static int hexdump(const uint8_t *b, size_t n, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < outlen; i++)
        o += snprintf(out + o, outlen - o, "%02x", b[i]);
    return (int)o;
}

/* Послать сообщение моста как output-репорт и дождаться ACK на input. */
static int bridge(const uint8_t op[2], const uint8_t *body, size_t body_len)
{
    uint8_t report[OUTPUT_LEN];
    memset(report, 0, sizeof report);
    report[0] = REPORT_ID;
    report[1] = op[0];
    report[2] = op[1];
    if (body_len)
        memcpy(report + 3, body, body_len);

    if (verbose) {
        char hx[64];
        hexdump(report, 3 + body_len, hx, sizeof hx);
        dbg("send %s", hx);
    }
    ssize_t w = write(g_fd, report, sizeof report);
    if (w < 0) {
        dbg("write: %s", strerror(errno));
        return HB_ERR_IO;
    }

    int64_t deadline = now_ms() + 800;
    uint8_t buf[64];
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = g_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0)
            continue;
        ssize_t n = read(g_fd, buf, sizeof buf);
        if (n >= 2 && buf[0] == REPORT_ID)
            return buf[1]; /* ACK */
    }
    return HB_ERR_NOREPLY;
}

/* GET_FEATURE репорта 6. Возвращает длину или -1. */
static int get_feature(uint8_t *buf)
{
    buf[0] = REPORT_ID;
    int n = ioctl(g_fd, HIDIOCGFEATURE(FEATURE_LEN), buf);
    return n;
}

/* Слить остатки прошлых ответов, чтобы не читать устаревшие данные. */
static void drain(void)
{
    uint8_t buf[FEATURE_LEN];
    for (int i = 0; i < 16; i++) {
        int n = get_feature(buf);
        if (n < 2 || buf[1] == 0xff)
            break;
    }
}

static uint8_t g_seq;

/* Послать команду "PA" и опрашивать feature-репорты, пока parse не совпадёт.
 * parse(buf, len, out) -> 1 при успехе. out — указатель на результат. */
typedef int (*parse_fn)(const uint8_t *, size_t, void *);

static int query(uint8_t cat, uint8_t len, const uint8_t *payload,
                 size_t payload_len, parse_fn parse, void *out)
{
    uint8_t cmd[32];
    size_t cmd_len = hb_pa_command(cmd, cat, g_seq, len, payload, payload_len);
    g_seq = (uint8_t)(g_seq % 127 + 1);

    /* тело: 16-битная длина команды + сама команда */
    uint8_t body[40];
    body[0] = (uint8_t)(cmd_len & 0xff);
    body[1] = (uint8_t)(cmd_len >> 8);
    memcpy(body + 2, cmd, cmd_len);

    int ack = bridge(OP_SEND, body, 2 + cmd_len);
    if (ack != ACK_OK && ack != ACK_ALREADY) {
        dbg("send ack=%d", ack);
        if (ack < 0)
            return ack;
    }

    int64_t deadline = now_ms() + 1500;
    uint8_t buf[FEATURE_LEN];
    while (now_ms() < deadline) {
        int n = get_feature(buf);
        if (n < 0) {
            dbg("get_feature: %s", strerror(errno));
            return HB_ERR_IO;
        }
        if (n >= 2 && buf[1] != 0xff) {
            if (verbose) {
                char hx[160];
                hexdump(buf, (size_t)n, hx, sizeof hx);
                dbg("reply %s", hx);
            }
            if (parse(buf, (size_t)n, out))
                return HB_OK;
            continue;
        }
        sleep_ms(30);
    }
    return HB_ERR_NOREPLY;
}

/* обёртки разбора под сигнатуру parse_fn */
static int p_remote_conn(const uint8_t *b, size_t n, void *out)
{
    return hb_parse_remote_value(b, n, HB_REMOTE_GET_CONNECT_STATUS,
                                 (uint8_t *)out);
}
static int p_battery_raw(const uint8_t *b, size_t n, void *out)
{
    return hb_parse_battery_raw(b, n, (uint16_t *)out);
}
static int p_any(const uint8_t *b, size_t n, void *out)
{
    (void)b; (void)n; (void)out;
    return 1; /* SET_MODE: достаточно любого ответа */
}

/* Прочитать милливольты. Всегда выходим из bootloader, даже при ошибке. */
static int read_millivolts(uint16_t *out_mv)
{
    int ack = bridge(OP_ENTER, NULL, 0);
    if (ack != ACK_OK && ack != ACK_ALREADY)
        dbg("enter ack=%d", ack);

    int rc = HB_ERR_NOREPLY;
    uint8_t conn = 0;
    uint16_t raw = 0;
    g_seq = 1;
    drain();

    /* GET_BATTERY никогда не первая команда: сначала статус подключения. */
    int q = query(HB_CAT_WIRELESS, 1, &(uint8_t){ HB_REMOTE_GET_CONNECT_STATUS },
                  1, p_remote_conn, &conn);
    if (q == HB_OK && conn == 0) {
        rc = HB_ERR_OFF;
        goto out;
    }

    q = query(HB_CAT_OTA, 1, HB_OTA_GET_BATTERY, 2, p_battery_raw, &raw);
    if (q == HB_OK && hb_is_battery_mv(raw)) {
        *out_mv = raw;
        rc = HB_OK;
        goto out;
    }

    /* В режиме 0 донгл отвечает мусором — включаем remote mode 1 и повторяем. */
    dbg("battery raw=%u, setting remote mode 1", raw);
    uint8_t setmode[2] = { HB_REMOTE_SET_MODE, 0x01 };
    query(HB_CAT_WIRELESS, 2, setmode, 2, p_any, NULL);
    q = query(HB_CAT_OTA, 1, HB_OTA_GET_BATTERY, 2, p_battery_raw, &raw);
    if (q == HB_OK && hb_is_battery_mv(raw)) {
        *out_mv = raw;
        rc = HB_OK;
    } else {
        rc = (q < 0) ? q : HB_ERR_NOREPLY;
    }

out:
    if (bridge(OP_EXIT, NULL, 0) < 0)
        dbg("exit bootloader failed");
    return rc;
}

/* Открыть узел (если ещё не открыт) и прочитать заряд. */
static int read_battery(uint16_t *out_mv, uint8_t *out_pct)
{
    char path[300];
    if (g_fd < 0) {
        if (find_dongle(path, sizeof path) != 0)
            return HB_ERR_NODEV;
        g_fd = open(path, O_RDWR);
        if (g_fd < 0) {
            dbg("open %s: %s", path, strerror(errno));
            return HB_ERR_OPEN;
        }
        dbg("opened %s", path);
    }
    uint16_t mv;
    int rc = read_millivolts(&mv);
    if (rc == HB_OK) {
        *out_mv = mv;
        *out_pct = hb_millivolts_to_percent(mv);
    } else if (rc == HB_ERR_IO && g_fd >= 0) {
        close(g_fd);
        g_fd = -1; /* попробуем переоткрыть в следующий раз */
    }
    return rc;
}

/* ================= проверка «играет ли звук» через pactl ================= */

/* 1 — если sink наушников сейчас RUNNING (звук идёт), иначе 0.
 * LC_ALL=C, чтобы вывод pactl не зависел от локали (имя/состояние на англ.). */
static int headset_playing(void)
{
    FILE *p = popen("LC_ALL=C pactl list sinks 2>/dev/null", "r");
    if (!p)
        return 0;
    char line[512];
    int block_running = 0, playing = 0;
    while (fgets(line, sizeof line, p)) {
        if (strncmp(line, "Sink #", 6) == 0)
            block_running = 0;
        if (strstr(line, "State: RUNNING"))
            block_running = 1;
        /* имя sink содержит G435; State: идёт раньше Name: в том же блоке */
        if (strstr(line, "Name:") && strstr(line, "G435") && block_running) {
            playing = 1;
            break;
        }
    }
    pclose(p);
    return playing;
}

/* ================= уведомления ================= */

static void notify(const char *summary, const char *body, int critical)
{
    dbg("notify: %s — %s", summary, body);
    /* notify-send есть в Debian по умолчанию; не тянем libnotify-dev. */
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "notify-send -a headbat -i audio-headphones %s '%s' '%s' >/dev/null 2>&1",
             critical ? "-u critical" : "-u normal", summary, body);
    if (system(cmd) != 0)
        dbg("notify-send failed");
}

/* ================= трей (Ayatana AppIndicator через dlopen) ================= */

/* Заголовки appindicator/gtk не обязательны для сборки: грузим библиотеки
 * через dlopen, как midmove грузит XTest. Если библиотек нет — работаем без трея. */
#include <dlfcn.h>

typedef void *(*app_indicator_new_fn)(const char *, const char *, int);
typedef void (*set_status_fn)(void *, int);
typedef void (*set_label_fn)(void *, const char *, const char *);
typedef void (*set_title_fn)(void *, const char *);
typedef void (*set_menu_fn)(void *, void *);
typedef void *(*menu_new_fn)(void);
typedef void *(*menu_item_new_fn)(const char *);
typedef void (*menu_shell_append_fn)(void *, void *);
typedef void (*widget_show_all_fn)(void *);
typedef void (*gtk_init_fn)(int *, char ***);
typedef void (*gtk_main_fn)(void);
typedef int  (*gtk_events_pending_fn)(void);
typedef int  (*gtk_main_iteration_fn)(void);
typedef unsigned long (*signal_connect_fn)(void *, const char *, void *,
                                           void *, void *, int);

static void on_quit(void *item, void *data)
{
    (void)item; (void)data;
    dbg("выход по меню трея");
    _exit(0);
}

static struct {
    void *lib_ind, *lib_gtk, *lib_gobj;
    app_indicator_new_fn new;
    set_status_fn set_status;
    set_label_fn set_label;
    set_title_fn set_title;
    set_menu_fn set_menu;
    menu_new_fn menu_new;
    menu_item_new_fn menu_item_new;
    menu_shell_append_fn menu_append;
    widget_show_all_fn show_all;
    gtk_init_fn gtk_init;
    gtk_main_fn gtk_main;
    gtk_events_pending_fn events_pending;
    gtk_main_iteration_fn main_iteration;
    signal_connect_fn signal_connect;
    void *indicator;
    int ready;
} tray;

#define APP_INDICATOR_CATEGORY_APPLICATIONS 0
#define APP_INDICATOR_STATUS_ACTIVE         1

static int tray_init(void)
{
    tray.lib_ind = dlopen("libayatana-appindicator3.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!tray.lib_ind)
        tray.lib_ind = dlopen("libappindicator3.so.1", RTLD_NOW | RTLD_GLOBAL);
    tray.lib_gtk = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    tray.lib_gobj = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!tray.lib_ind || !tray.lib_gtk || !tray.lib_gobj) {
        dbg("трей недоступен: %s", dlerror());
        return -1;
    }
    tray.new = (app_indicator_new_fn)dlsym(tray.lib_ind, "app_indicator_new");
    tray.set_status = (set_status_fn)dlsym(tray.lib_ind, "app_indicator_set_status");
    tray.set_label = (set_label_fn)dlsym(tray.lib_ind, "app_indicator_set_label");
    tray.set_title = (set_title_fn)dlsym(tray.lib_ind, "app_indicator_set_title");
    tray.set_menu = (set_menu_fn)dlsym(tray.lib_ind, "app_indicator_set_menu");
    tray.menu_new = (menu_new_fn)dlsym(tray.lib_gtk, "gtk_menu_new");
    tray.menu_item_new = (menu_item_new_fn)dlsym(tray.lib_gtk, "gtk_menu_item_new_with_label");
    tray.menu_append = (menu_shell_append_fn)dlsym(tray.lib_gtk, "gtk_menu_shell_append");
    tray.show_all = (widget_show_all_fn)dlsym(tray.lib_gtk, "gtk_widget_show_all");
    tray.gtk_init = (gtk_init_fn)dlsym(tray.lib_gtk, "gtk_init");
    tray.gtk_main = (gtk_main_fn)dlsym(tray.lib_gtk, "gtk_main");
    tray.events_pending = (gtk_events_pending_fn)dlsym(tray.lib_gtk, "gtk_events_pending");
    tray.main_iteration = (gtk_main_iteration_fn)dlsym(tray.lib_gtk, "gtk_main_iteration");
    tray.signal_connect = (signal_connect_fn)dlsym(tray.lib_gobj, "g_signal_connect_data");
    if (!tray.new || !tray.set_status || !tray.set_label || !tray.set_menu ||
        !tray.menu_new || !tray.menu_item_new || !tray.menu_append ||
        !tray.gtk_init || !tray.events_pending || !tray.main_iteration ||
        !tray.signal_connect) {
        dbg("не все символы appindicator найдены");
        return -1;
    }

    int argc = 1;
    char *argv0 = (char *)"headbat";
    char **argv = &argv0;
    tray.gtk_init(&argc, &argv);
    tray.indicator = tray.new("headbat", "audio-headphones",
                              APP_INDICATOR_CATEGORY_APPLICATIONS);
    if (!tray.indicator)
        return -1;
    if (tray.set_title)
        tray.set_title(tray.indicator, "Logitech G435");

    /* Меню обязательно: без него libayatana-appindicator не регистрирует
     * индикатор в StatusNotifierWatcher (проверено на 0.5.94). */
    void *menu = tray.menu_new();
    void *item = tray.menu_item_new("Выход");
    tray.signal_connect(item, "activate", (void *)on_quit, NULL, NULL, 0);
    tray.menu_append(menu, item);
    if (tray.show_all)
        tray.show_all(menu);
    tray.set_menu(tray.indicator, menu);

    tray.set_status(tray.indicator, APP_INDICATOR_STATUS_ACTIVE);
    tray.set_label(tray.indicator, "…", "");
    tray.ready = 1;
    dbg("трей запущен");
    return 0;
}

static int g_label_toggle;   /* чередуем завершающий пробел, чтобы libayatana
                              * всё-таки слала PropertiesChanged при том же % */

static void tray_update(int pct, int connected)
{
    if (!tray.ready)
        return;
    char label[32];
    if (connected && pct >= 0)
        snprintf(label, sizeof label, "%d%%%s", pct,
                 (g_label_toggle++ & 1) ? " " : "");
    else
        snprintf(label, sizeof label, "—");
    tray.set_label(tray.indicator, label, "");
    /* Дублируем в Title: если тема/extension прячет XAyatanaLabel,
     * заряд всё равно виден в подсказке и меню индикатора. */
    if (tray.set_title)
        tray.set_title(tray.indicator, label);
}

static void tray_pump(void)
{
    if (!tray.ready)
        return;
    while (tray.events_pending())
        tray.main_iteration();
}

/* ================= главный цикл ================= */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Использование: %s [опции]\n"
        "  --interval N   секунд между опросами (по умолчанию %d)\n"
        "  --status       один раз вывести заряд и выйти\n"
        "  --no-tray      без иконки в трее\n"
        "  --always       не пропускать опрос, когда играет звук\n"
        "  --max-silence-wait N  ждать тишины не дольше N сек (по умолч. %d)\n"
        "  -v, --verbose  подробный лог в stderr\n"
        "  -h, --help     эта справка\n",
        prog, DEFAULT_INTERVAL, DEFAULT_MAX_SILENCE_WAIT);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interval") && i + 1 < argc)
            opt_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--status"))
            opt_status = 1;
        else if (!strcmp(argv[i], "--no-tray"))
            opt_no_tray = 1;
        else if (!strcmp(argv[i], "--always"))
            opt_always = 1;
        else if (!strcmp(argv[i], "--max-silence-wait") && i + 1 < argc)
            opt_max_silence_wait = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Неизвестная опция: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (opt_interval < 1)
        opt_interval = 1;
    if (opt_max_silence_wait < 1)
        opt_max_silence_wait = 1;

    /* разовый режим */
    if (opt_status) {
        uint16_t mv; uint8_t pct;
        int rc = read_battery(&mv, &pct);
        switch (rc) {
        case HB_OK:
            printf("%u%% (%u мВ)\n", pct, mv);
            return 0;
        case HB_ERR_NODEV:
            fprintf(stderr, "донгл G435 не найден\n"); return 1;
        case HB_ERR_OPEN:
            fprintf(stderr, "нет доступа к /dev/hidraw* (нужен udev-rule, см. README)\n"); return 1;
        case HB_ERR_OFF:
            printf("наушники выключены или не подключены\n"); return 0;
        default:
            fprintf(stderr, "не удалось прочитать заряд (код %d)\n", rc); return 1;
        }
    }

    if (!opt_no_tray)
        tray_init(); /* если не вышло — работаем без трея */

    int last_pct = -1;
    int was_connected = 0;
    int warned_low = 0, warned_crit = 0;
    int have_read = 0;          /* был ли хоть один успешный опрос */
    int64_t silent_since = 0;   /* когда звук начался (для таймаута ожидания) */

    dbg("старт: интервал %d с, макс. ожидание тишины %d с%s",
        opt_interval, opt_max_silence_wait, opt_always ? ", всегда" : "");

    for (;;) {
        tray_pump();

        /* Первый опрос — сразу при старте, даже если играет звук: иначе трей
         * пустой до первой паузы (до interval секунд). Дальше при звуке
         * пропускаем, но не дольше opt_max_silence_wait сек — как в g435-tray:
         * лучше пожертвовать 0.6 с звука, чем показывать устаревший заряд. */
        int playing = headset_playing();
        if (!playing)
            silent_since = 0;
        else if (silent_since == 0)
            silent_since = now_ms();
        int waited_too_long = playing && silent_since != 0 &&
            now_ms() - silent_since >= (int64_t)opt_max_silence_wait * 1000;
        int skip = have_read && !opt_always && playing && !waited_too_long;
        if (skip) {
            dbg("звук идёт — опрос пропущен (тишины ждём %lld с)",
                (long long)((now_ms() - silent_since) / 1000));
        } else {
            uint16_t mv; uint8_t pct;
            int rc = read_battery(&mv, &pct);
            if (rc == HB_OK) {
                dbg("заряд %u%% (%u мВ)", pct, mv);
                have_read = 1;
                tray_update(pct, 1);
                if (!was_connected) {
                    char body[64];
                    snprintf(body, sizeof body, "Подключены, заряд %u%%", pct);
                    notify("Наушники G435", body, 0);
                }
                was_connected = 1;
                last_pct = pct;

                if (pct <= LOW_CRIT && !warned_crit) {
                    char body[64];
                    snprintf(body, sizeof body, "Заряд %u%% — скоро отключатся", pct);
                    notify("Низкий заряд G435", body, 1);
                    warned_crit = 1; warned_low = 1;
                } else if (pct <= LOW_WARN && !warned_low) {
                    char body[64];
                    snprintf(body, sizeof body, "Заряд %u%%", pct);
                    notify("Низкий заряд G435", body, 0);
                    warned_low = 1;
                }
                if (pct > LOW_WARN) { warned_low = 0; warned_crit = 0; }
            } else if (rc == HB_ERR_OFF) {
                have_read = 1; /* опрос состоялся: наушники просто выключены */
                tray_update(-1, 0);
                if (was_connected)
                    notify("Наушники G435", "Отключены или выключены", 0);
                was_connected = 0;
                last_pct = -1;
            } else {
                dbg("чтение не удалось (код %d)", rc);
                tray_update(last_pct, was_connected);
            }
        }

        /* Спим интервал (10 с после неудачного старта), прокачивая GTK.
         * Раз в 2 с повторно выставляем подпись: расширение appindicators
         * подхватывает XAyatanaLabel по PropertiesChanged, но если метка
         * пришла до того, как оно добавило опциональное свойство в список,
         * первый persists без повторa. Повтор дешёвый и без HID-опроса. */
        int wait_s = have_read ? opt_interval : 10;
        int64_t end = now_ms() + (int64_t)wait_s * 1000;
        int64_t next_label = now_ms() + 2000;
        while (now_ms() < end) {
            tray_pump();
            if (tray.ready && now_ms() >= next_label) {
                tray_update(last_pct, was_connected);
                next_label = now_ms() + 2000;
            }
            sleep_ms(200);
        }
    }
    return 0;
}