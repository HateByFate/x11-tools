/*
 * headbat.h — протокол чтения заряда Logitech G435 (донгл 046d:0acb).
 *
 * Чистые функции без I/O: сборка команд "PA", разбор ответов "PI",
 * кривая напряжение→проценты. Вынесено отдельно, чтобы покрыть тестами.
 *
 * Протокол и кривая портированы из проекта deddroid/logitech-g435-battery
 * (MIT), который опирается на реверс endes (dev.to «USB HID down the rabbit
 * hole») и утилиту обновления Logitech G435Update. См. README.md.
 */
#ifndef HEADBAT_H
#define HEADBAT_H

#include <stddef.h>
#include <stdint.h>

/* Категории команд "PA" (байт 2). */
#define HB_CAT_OTA      0x06
#define HB_CAT_WIRELESS 0x0e

/* Команды беспроводной категории. */
#define HB_REMOTE_GET_MODE           0xe0
#define HB_REMOTE_SET_MODE           0xe1
#define HB_REMOTE_GET_CONNECT_STATUS 0xe3

/* GET_BATTERY (0x0031) из OTA-категории; ответ несёт милливольты. */
extern const uint8_t HB_OTA_GET_BATTERY[2];

/* Собирает команду "PA": 50 41 <cat> <seq> <len> <payload...>.
 * Возвращает длину (5 + payload_len). buf должен быть >= 5 + payload_len. */
size_t hb_pa_command(uint8_t *buf, uint8_t cat, uint8_t seq, uint8_t len,
                     const uint8_t *payload, size_t payload_len);

/* Ищет в буфере ответ GET_BATTERY вида 07 00 06 <seq|80> 00 <mv_lo> <mv_hi>
 * и возвращает сырое значение. 1 — найдено, 0 — нет. */
int hb_parse_battery_raw(const uint8_t *buf, size_t len, uint16_t *out_mv);

/* Правдоподобное напряжение Li-Po аккумулятора наушников. */
int hb_is_battery_mv(uint16_t mv);

/* Ответ GET_BATTERY с правдоподобным напряжением (сырое + фильтр). */
int hb_parse_battery_mv(const uint8_t *buf, size_t len, uint16_t *out_mv);

/* Значение ответа беспроводной категории: 02 00 <cmd> <value>. */
int hb_parse_remote_value(const uint8_t *buf, size_t len, uint8_t cmd,
                          uint8_t *out_value);

/* Приблизительный заряд по напряжению покоя Li-Po (кусочная кривая). */
uint8_t hb_millivolts_to_percent(uint16_t mv);

#endif /* HEADBAT_H */