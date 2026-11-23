/*
 * protocol_test.c — автотесты чистых функций протокола G435.
 * Кейсы портированы из deddroid/logitech-g435-battery (MIT), src/protocol.rs.
 * Не требует железа: только разбор буферов и кривая процентов.
 *
 * Сборка: gcc -O2 -Wall -Wextra -o protocol_test protocol_test.c protocol.c
 */
#include <stdio.h>
#include <string.h>
#include "../headbat.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* hex-строка -> байты */
static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; s[i] && s[i + 1] && n < cap; i += 2) {
        unsigned b;
        sscanf(s + i, "%2x", &b);
        out[n++] = (uint8_t)b;
    }
    return n;
}

int main(void)
{
    uint8_t buf[64];
    size_t n;
    uint16_t mv;
    uint8_t val;

    /* ответ GET_BATTERY с реальным напряжением (0acb, feature report 6) */
    n = unhex("06504901c875480d00070006820032100000ffffff", buf, sizeof buf);
    CHECK(hb_parse_battery_mv(buf, n, &mv) == 1);
    CHECK(mv == 4146);

    /* тот же ответ в неверном режиме: 210 — не напряжение */
    n = unhex("06504901c0d64300000700068200d2000000ffff", buf, sizeof buf);
    CHECK(hb_parse_battery_mv(buf, n, &mv) == 0);
    CHECK(hb_parse_battery_raw(buf, n, &mv) == 1);
    CHECK(mv == 210);

    /* беспроводные ответы */
    n = unhex("0650490ec7000000000200e001ffff", buf, sizeof buf);
    CHECK(hb_parse_remote_value(buf, n, HB_REMOTE_GET_MODE, &val) == 1);
    CHECK(val == 1);
    n = unhex("0650490ec2000000000200e301ffff", buf, sizeof buf);
    CHECK(hb_parse_remote_value(buf, n, HB_REMOTE_GET_CONNECT_STATUS, &val) == 1);
    CHECK(val == 1);
    CHECK(hb_parse_remote_value(buf, n, HB_REMOTE_GET_MODE, &val) == 0);

    /* кривая процентов */
    CHECK(hb_millivolts_to_percent(4300) == 100);
    CHECK(hb_millivolts_to_percent(4146) == 95);
    CHECK(hb_millivolts_to_percent(3725) == 40);
    CHECK(hb_millivolts_to_percent(3300) == 0);

    /* сборка команды PA */
    uint8_t cmd[16];
    size_t cl = hb_pa_command(cmd, HB_CAT_OTA, 2, 1, HB_OTA_GET_BATTERY, 2);
    CHECK(cl == 7);
    CHECK(cmd[0] == 0x50 && cmd[1] == 0x41 && cmd[2] == HB_CAT_OTA);
    CHECK(cmd[3] == 2 && cmd[4] == 1 && cmd[5] == 0x00 && cmd[6] == 0x31);

    if (failures) {
        fprintf(stderr, "%d проверок провалено\n", failures);
        return 1;
    }
    printf("protocol: все проверки пройдены\n");
    return 0;
}