#include "tuya_uart.h"

static uint8_t checksum(const uint8_t *data, uint16_t length)
{
    uint8_t sum = 0;
    while (length--) {
        sum += *data++;
    }
    return sum;
}

static void discard(tuya_parser_t *parser, uint16_t count)
{
    parser->used -= count;
    for (uint16_t i = 0; i < parser->used; ++i) {
        parser->data[i] = parser->data[i + count];
    }
}

void tuya_feed(tuya_parser_t *parser, uint8_t byte, tuya_frame_cb callback)
{
    parser->data[parser->used++] = byte;
    while (parser->used) {
        uint8_t *data = parser->data;
        if (data[0] != 0x55 || (parser->used >= 2 && data[1] != 0xaa)) {
            discard(parser, 1);
            continue;
        }
        if (parser->used < 6) {
            return;
        }
        const uint16_t length = ((uint16_t)data[4] << 8) | data[5];
        if (data[2] != 0 || length > TUYA_MAX_PAYLOAD) {
            discard(parser, 1);
            continue;
        }
        const uint16_t size = length + TUYA_FRAME_OVERHEAD;
        if (parser->used < size) {
            return;
        }
        if (checksum(data, size - 1) != data[size - 1]) {
            discard(parser, 1);
            continue;
        }
        callback(data[3], data + 6, length);
        discard(parser, size);
    }
}

uint16_t tuya_encode(uint8_t *frame, uint8_t command, const uint8_t *payload, uint16_t length)
{
    frame[0] = 0x55;
    frame[1] = 0xaa;
    frame[2] = 0;
    frame[3] = command;
    frame[4] = length >> 8;
    frame[5] = length;
    for (uint16_t i = 0; i < length; ++i) {
        frame[6 + i] = payload[i];
    }
    frame[6 + length] = checksum(frame, 6 + length);
    return length + TUYA_FRAME_OVERHEAD;
}

uint16_t tuya_valve_command(uint8_t *frame, uint8_t on, uint32_t seconds)
{
    const uint8_t dps[] = {
        1, 1, 0, 1, on != 0,
        11, 2, 0, 4, seconds >> 24, seconds >> 16, seconds >> 8, seconds,
    };
    return tuya_encode(frame, TUYA_WRITE_DPS, dps, on ? sizeof(dps) : 5);
}
