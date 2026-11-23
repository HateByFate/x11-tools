/*
 * protocol.c — чистые функции протокола G435 (без I/O), см. headbat.h.
 * Порт из deddroid/logitech-g435-battery (MIT), src/protocol.rs.
 */
#include "headbat.h"

const uint8_t HB_OTA_GET_BATTERY[2] = { 0x00, 0x31 };

size_t hb_pa_command(uint8_t *buf, uint8_t cat, uint8_t seq, uint8_t len,
                     const uint8_t *payload, size_t payload_len)
{
    buf[0] = 0x50; /* 'P' */
    buf[1] = 0x41; /* 'A' */
    buf[2] = cat;
    buf[3] = seq;
    buf[4] = len;
    for (size_t i = 0; i < payload_len; i++)
        buf[5 + i] = payload[i];
    return 5 + payload_len;
}

int hb_parse_battery_raw(const uint8_t *buf, size_t len, uint16_t *out_mv)
{
    if (len < 7)
        return 0;
    for (size_t i = 0; i + 7 <= len; i++) {
        const uint8_t *w = buf + i;
        if (w[0] == 0x07 && w[1] == 0x00 && w[2] == HB_CAT_OTA &&
            (w[3] & 0x80) != 0 && w[4] == 0x00) {
            *out_mv = (uint16_t)(w[5] | (w[6] << 8));
            return 1;
        }
    }
    return 0;
}

int hb_is_battery_mv(uint16_t mv)
{
    return mv >= 2500 && mv <= 4600;
}

int hb_parse_battery_mv(const uint8_t *buf, size_t len, uint16_t *out_mv)
{
    uint16_t mv;
    if (!hb_parse_battery_raw(buf, len, &mv))
        return 0;
    if (!hb_is_battery_mv(mv))
        return 0;
    *out_mv = mv;
    return 1;
}

int hb_parse_remote_value(const uint8_t *buf, size_t len, uint8_t cmd,
                          uint8_t *out_value)
{
    if (len < 4)
        return 0;
    for (size_t i = 0; i + 4 <= len; i++) {
        const uint8_t *w = buf + i;
        if (w[0] == 0x02 && w[1] == 0x00 && w[2] == cmd) {
            *out_value = w[3];
            return 1;
        }
    }
    return 0;
}

uint8_t hb_millivolts_to_percent(uint16_t mv)
{
    static const struct { uint16_t mv; uint8_t pct; } curve[] = {
        { 4200, 100 }, { 4150, 95 }, { 4100, 90 }, { 4050, 85 },
        { 4000, 80 },  { 3950, 75 }, { 3900, 70 }, { 3850, 62 },
        { 3800, 55 },  { 3750, 45 }, { 3700, 35 }, { 3650, 25 },
        { 3600, 15 },  { 3550, 10 }, { 3500, 5 },  { 3400, 0 },
    };
    const size_t n = sizeof curve / sizeof curve[0];

    if (mv >= curve[0].mv)
        return 100;
    for (size_t i = 0; i + 1 < n; i++) {
        uint16_t hi_mv = curve[i].mv, lo_mv = curve[i + 1].mv;
        uint8_t hi_pct = curve[i].pct, lo_pct = curve[i + 1].pct;
        if (mv >= lo_mv) {
            float t = (float)(mv - lo_mv) / (float)(hi_mv - lo_mv);
            float p = (float)lo_pct + t * (float)(hi_pct - lo_pct);
            return (uint8_t)(p + 0.5f);
        }
    }
    return 0;
}