#pragma once

#include <stdint.h>

enum {
    TUYA_HEARTBEAT = 0x00,
    TUYA_MCU_INFO = 0x01,
    TUYA_WORK_MODE = 0x02,
    TUYA_CONNECTION_STATE = 0x03,
    TUYA_WRITE_DPS = 0x06,
    TUYA_REPORT_DPS = 0x07,
    TUYA_QUERY_DPS = 0x08,
    TUYA_JOIN_BUTTON = 0x09,
    TUYA_ADV_INTERVAL = 0xe2,
    TUYA_LOW_POWER = 0xe5,
    TUYA_MCU_VERSION = 0xe8,
};

enum { TUYA_FRAME_OVERHEAD = 7, TUYA_MAX_PAYLOAD = 128 };

typedef struct {
    uint8_t data[TUYA_MAX_PAYLOAD + TUYA_FRAME_OVERHEAD];
    uint16_t used;
} tuya_parser_t;

typedef void (*tuya_frame_cb)(uint8_t command, const uint8_t *payload, uint16_t length);

void tuya_feed(tuya_parser_t *parser, uint8_t byte, tuya_frame_cb callback);
uint16_t tuya_encode(uint8_t *frame, uint8_t command, const uint8_t *payload, uint16_t length);
uint16_t tuya_valve_command(uint8_t *frame, uint8_t on, uint32_t seconds);
