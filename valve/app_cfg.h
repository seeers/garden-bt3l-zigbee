#pragma once

#include "version_cfg.h"

#define CLOCK_SYS_CLOCK_HZ 24000000
#define UART_CLOCK_SOURCE CLOCK_SYS_CLOCK_HZ
#define UART_PRINTF_MODE 0
#define ZBHCI_UART 0
#define FLASH_PROTECT_ENABLE 0
#define VOLTAGE_DETECT_ENABLE 0
#define VOLTAGE_DETECT_ADC_PIN GPIO_PC5
#define PM_ENABLE 1
#define MODULE_WATCHDOG_ENABLE 0
#define TOUCHLINK_SUPPORT 0
#define FIND_AND_BIND_SUPPORT 0
#define ZCL_ON_OFF_SUPPORT 1
#define ZCL_POWER_CFG_SUPPORT 1
#define ZCL_OTA_SUPPORT 0

typedef enum {
    EV_POLL_ED_DETECT,
    EV_POLL_HCI,
    EV_POLL_IDLE,
    EV_POLL_MAX,
} ev_poll_e;

#include "zb_config.h"
#include "../src/stack_cfg.h"
