#include "tl_common.h"
#include "zb_common.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "bdb.h"
#include "zigbee/aps/aps_api.h"
#include "tuya_uart.h"

#define EP 1
#define DEFAULT_RUN_SECONDS (60u * 60u)
#define MAX_RUN_SECONDS (6u * 60u * 60u)
#define GARDEN_CONTROL_CLUSTER 0xfc90
#define GARDEN_START_TIMED 0x00
#define RX_QUEUE_SIZE 512
#define TX_QUEUE_SIZE 16
#define POLL_INTERVAL_MS 3000
#define MCU_WAKE_SETTLE_US 12000
#define UART_IDLE_US 30000

// Original BT3L firmware: PB5 wakes the module, PD2 wakes the FT61E0A5.
#define MODULE_WAKE_PIN GPIO_PB5
#define MCU_WAKE_PIN GPIO_PD2

typedef struct {
    u32 length;
    u8 data[20];
} uart_tx_packet_t;

static volatile u8 rx_dma[260] __attribute__((aligned(4)));
static volatile u8 rx_queue[RX_QUEUE_SIZE];
static volatile u16 rx_in, rx_out;
static volatile u8 rx_overflow;
static uart_tx_packet_t tx_dma __attribute__((aligned(4)));
static uart_tx_packet_t tx_queue[TX_QUEUE_SIZE];
static u8 tx_in, tx_out;
static u8 mcu_wake_active;
static u32 mcu_wake_tick;
static u8 low_power_enabled;
static u8 module_wake_seen_low;
static u8 module_wake_seen_high;
static u8 module_wake_verified;
static u32 module_wake_tick;
static drv_pm_pinCfg_t wake_pin = {MODULE_WAKE_PIN, PM_WAKEUP_LEVEL_HIGH};
static tuya_parser_t parser;

static u8 valve_on;
static u8 valve_known;
static u8 battery = 0xff;
static u8 own_network;
static u8 join_requested;
static u8 startup_step;
static u32 startup_tick;
static u32 command_tick;
static u8 command_pending;
static u8 command_target;
static u32 last_rx_tick;
static u32 rejoin_tick;
static u8 rejoin_pending;

static u8 zcl_version = 3;
static u8 app_version = 4;
static u8 power_source = 3;
static u8 manufacturer[] = {4, 'A', 'L', 'D', 'I'};
static u8 model[] = {13, 'G','A','R','D','E','N','-','B','T','3','L','-','Z'};
static u16 identify_time;

#define READ_ATTR(id, type, data) {id, type, ACCESS_CONTROL_READ, (u8 *)(data)}
#define REPORT_ATTR(id, type, data) {id, type, ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8 *)(data)}
#define NUM(a) (sizeof(a) / sizeof((a)[0]))

static const zclAttrInfo_t basic_attrs[] = {
    READ_ATTR(ZCL_ATTRID_BASIC_ZCL_VER, ZCL_DATA_TYPE_UINT8, &zcl_version),
    READ_ATTR(ZCL_ATTRID_BASIC_APP_VER, ZCL_DATA_TYPE_UINT8, &app_version),
    READ_ATTR(ZCL_ATTRID_BASIC_MFR_NAME, ZCL_DATA_TYPE_CHAR_STR, manufacturer),
    READ_ATTR(ZCL_ATTRID_BASIC_MODEL_ID, ZCL_DATA_TYPE_CHAR_STR, model),
    READ_ATTR(ZCL_ATTRID_BASIC_POWER_SOURCE, ZCL_DATA_TYPE_ENUM8, &power_source),
};

static const zclAttrInfo_t identify_attrs[] = {
    {ZCL_ATTRID_IDENTIFY_TIME, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE, (u8 *)&identify_time},
};

static const zclAttrInfo_t onoff_attrs[] = {
    REPORT_ATTR(ZCL_ATTRID_ONOFF, ZCL_DATA_TYPE_BOOLEAN, &valve_on),
};

static const zclAttrInfo_t battery_attrs[] = {
    REPORT_ATTR(ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING, ZCL_DATA_TYPE_UINT8, &battery),
};

static u16 in_clusters[] = {
    ZCL_CLUSTER_GEN_BASIC, ZCL_CLUSTER_GEN_IDENTIFY,
    ZCL_CLUSTER_GEN_ON_OFF, ZCL_CLUSTER_GEN_POWER_CFG, GARDEN_CONTROL_CLUSTER,
};
static af_simple_descriptor_t descriptor = {
    HA_PROFILE_ID, HA_DEV_ONOFF_PLUG_IN_UNIT, EP, 1, 0, NUM(in_clusters), 0, in_clusters, NULL,
};

static void rx_irq(void)
{
    u32 length = rx_dma[0] | ((u32)rx_dma[1] << 8) |
                 ((u32)rx_dma[2] << 16) | ((u32)rx_dma[3] << 24);
    if (length > sizeof(rx_dma) - 4) {
        rx_overflow = 1;
        return;
    }
    for (u32 i = 0; i < length; ++i) {
        u16 next = (rx_in + 1) & (RX_QUEUE_SIZE - 1);
        if (next == rx_out) {
            rx_overflow = 1;
            return;
        }
        rx_queue[rx_in] = rx_dma[4 + i];
        rx_in = next;
    }
}

static uart_tx_packet_t *tx_alloc(void)
{
    if ((tx_in + 1) % TX_QUEUE_SIZE == tx_out) {
        return NULL;
    }
    return &tx_queue[tx_in];
}

static void tx_commit(void)
{
    tx_in = (tx_in + 1) % TX_QUEUE_SIZE;
}

static u8 send_frame(u8 command, const u8 *payload, u16 length)
{
    uart_tx_packet_t *packet = tx_alloc();
    if (!packet || length + TUYA_FRAME_OVERHEAD > sizeof(packet->data)) {
        return 0;
    }
    packet->length = tuya_encode(packet->data, command, payload, length);
    tx_commit();
    return 1;
}

static void tx_poll(void)
{
    if (tx_out == tx_in) {
        if (mcu_wake_active && !uart_tx_is_busy()) {
            gpio_write(MCU_WAKE_PIN, 0);
            mcu_wake_active = 0;
        }
        return;
    }
    if (!mcu_wake_active) {
        gpio_write(MCU_WAKE_PIN, 1);
        mcu_wake_tick = clock_time();
        mcu_wake_active = 1;
        return;
    }
    if (!clock_time_exceed(mcu_wake_tick, MCU_WAKE_SETTLE_US) || uart_tx_is_busy()) {
        return;
    }
    tx_dma = tx_queue[tx_out];
    if (uart_dma_send((u8 *)&tx_dma)) {
        tx_out = (tx_out + 1) % TX_QUEUE_SIZE;
    }
}

static void observe_module_wake(void)
{
    if (!gpio_read(MODULE_WAKE_PIN)) {
        module_wake_seen_low = 1;
    } else if (module_wake_seen_low) {
        module_wake_seen_high = 1;
        module_wake_tick = clock_time();
    }
}

static void report_dps(const u8 *data, u16 length)
{
    if (length < 4) {
        return;
    }
    const u8 *cursor = data;
    u16 left = length;
    u8 next_on = valve_on;
    u8 on_found = 0;
    u8 next_battery = battery;
    while (left >= 4) {
        const u16 size = ((u16)cursor[2] << 8) | cursor[3];
        if (size > left - 4) {
            return;
        }
        if (cursor[0] == 1 && cursor[1] == 1 && size == 1 && cursor[4] <= 1) {
            next_on = cursor[4];
            on_found = 1;
        } else if (cursor[0] == 7 && cursor[1] == 2 && size == 4 &&
                   !cursor[4] && !cursor[5] && !cursor[6] && cursor[7] <= 100) {
            next_battery = 2 * cursor[7];
        }
        cursor += 4 + size;
        left -= 4 + size;
    }
    if (left) {
        return;
    }
    if (on_found) {
        valve_on = next_on;
        valve_known = 1;
        if (valve_on == command_target) {
            command_pending = 0;
        }
    }
    battery = next_battery;
    const u8 ok = 0;
    send_frame(TUYA_REPORT_DPS, &ok, 1);
}

static void receive_frame(u8 command, const u8 *payload, u16 length)
{
    // Require a real wake pulse near a valid UART frame before using suspend.
    if (module_wake_seen_high && !clock_time_exceed(module_wake_tick, UART_IDLE_US)) {
        module_wake_verified = 1;
    }
    const u8 ok = 0;
    switch (command) {
    case TUYA_REPORT_DPS:
        report_dps(payload, length);
        break;
    case TUYA_JOIN_BUTTON:
        if (length == 1 && payload[0] == 0) {
            if (send_frame(command, &ok, 1) && !own_network) {
                join_requested = 1;
            }
        }
        break;
    case TUYA_ADV_INTERVAL:
        if (length == 1) {
            send_frame(command, &ok, 1);
        }
        break;
    case TUYA_LOW_POWER:
        if (length == 1 && payload[0] <= 1 && send_frame(command, &ok, 1)) {
            low_power_enabled = payload[0];
        }
        break;
    }
}

static void uart_poll(void)
{
    observe_module_wake();
    if (rx_overflow) {
        const u8 irq = irq_disable();
        rx_out = rx_in;
        rx_overflow = 0;
        irq_restore(irq);
        parser.used = 0;
    }
    if (parser.used && clock_time_exceed(last_rx_tick, 50000)) {
        parser.used = 0;
    }
    while (rx_out != rx_in) {
        u8 byte = rx_queue[rx_out];
        rx_out = (rx_out + 1) & (RX_QUEUE_SIZE - 1);
        last_rx_tick = clock_time();
        tuya_feed(&parser, byte, receive_frame);
    }
    tx_poll();
}

static status_t queue_valve_command(u8 on, u32 seconds)
{
    if (on && (!seconds || seconds > MAX_RUN_SECONDS)) {
        return ZCL_STA_INVALID_VALUE;
    }
    if (on && (!valve_known || command_pending)) {
        return ZCL_STA_FAILURE;
    }
    uart_tx_packet_t *packet = tx_alloc();
    if (!packet) {
        return ZCL_STA_FAILURE;
    }
    packet->length = tuya_valve_command(packet->data, on, seconds);
    tx_commit();
    command_target = on;
    command_pending = 1;
    command_tick = clock_time();
    return ZCL_STA_SUCCESS;
}

static status_t onoff_command(zclIncoming_t *message)
{
    if (message->hdr.frmCtrl.bf.dir != ZCL_FRAME_CLIENT_SERVER_DIR) {
        return ZCL_STA_UNSUP_CLUSTER_COMMAND;
    }
    const u8 cmd = message->hdr.cmd;
    if (message->dataLen != (cmd == ZCL_CMD_ON_WITH_TIMED_OFF ? 5 : 0)) {
        return ZCL_STA_MALFORMED_COMMAND;
    }
    u8 on = 0;
    u32 seconds = DEFAULT_RUN_SECONDS;
    switch (cmd) {
    case ZCL_CMD_ONOFF_OFF:
        break;
    case ZCL_CMD_ONOFF_ON:
        on = 1;
        break;
    case ZCL_CMD_ONOFF_TOGGLE:
        if (!valve_known || command_pending) {
            return ZCL_STA_FAILURE;
        }
        on = !valve_on;
        break;
    case ZCL_CMD_ON_WITH_TIMED_OFF: {
        const u8 *data = message->pData;
        const u16 on_time = data[1] | ((u16)data[2] << 8);
        if (data[0] || data[3] || data[4] ||
            !on_time || on_time == 0xffff) {
            return ZCL_STA_INVALID_VALUE;
        }
        on = 1;
        seconds = (on_time + 9) / 10;
        break;
    }
    default:
        return ZCL_STA_UNSUP_CLUSTER_COMMAND;
    }
    return queue_valve_command(on, seconds);
}

static status_t garden_control_command(zclIncoming_t *message)
{
    if (message->hdr.frmCtrl.bf.dir != ZCL_FRAME_CLIENT_SERVER_DIR ||
        message->hdr.cmd != GARDEN_START_TIMED) {
        return ZCL_STA_UNSUP_CLUSTER_COMMAND;
    }
    if (message->dataLen != 4) {
        return ZCL_STA_MALFORMED_COMMAND;
    }
    const u8 *data = message->pData;
    const u32 seconds = (u32)data[0] | ((u32)data[1] << 8) |
                        ((u32)data[2] << 16) | ((u32)data[3] << 24);
    return queue_valve_command(1, seconds);
}

static void zcl_hook(zclIncoming_t *message)
{
    (void)message;
}

static void bdb_initialized(u8 status, u8 joined)
{
    if (joined) {
        own_network = 1;
        if (status == BDB_INIT_STATUS_SUCCESS) {
            zb_setPollRate(POLL_INTERVAL_MS);
        } else {
            rejoin_tick = clock_time();
            rejoin_pending = 1;
        }
    }
}

static void bdb_commissioned(u8 status, void *arg)
{
    (void)arg;
    if (status == BDB_COMMISSION_STA_SUCCESS && zb_isDeviceJoinedNwk()) {
        own_network = 1;
        rejoin_pending = 0;
        zb_setPollRate(POLL_INTERVAL_MS);
        send_frame(TUYA_QUERY_DPS, NULL, 0);
    } else if (own_network && (status == BDB_COMMISSION_STA_PARENT_LOST ||
                              status == BDB_COMMISSION_STA_REJOIN_FAILURE)) {
        rejoin_tick = clock_time();
        rejoin_pending = 1;
    }
}

static bdb_appCb_t bdb_callbacks = {
    bdb_initialized, bdb_commissioned, NULL, NULL,
};

static void network_left(nlme_leave_ind_t *indication)
{
    if (!indication->rejoin) {
        own_network = 0;
        rejoin_pending = 0;
        join_requested = 0;
    }
}

static zdo_appIndCb_t zdo_callbacks = {
    .zdpStartDevCnfCb = bdb_zdoStartDevCnf,
    .zdpLeaveIndCb = network_left,
};

static bdb_commissionSetting_t commission = {
    .linkKey.tcLinkKey.keyType = SS_GLOBAL_LINK_KEY,
    .linkKey.tcLinkKey.key = (u8 *)tcLinkKeyCentralDefault,
    .linkKey.distributeLinkKey.keyType = MASTER_KEY,
    .linkKey.distributeLinkKey.key = (u8 *)linkKeyDistributedMaster,
};

static const u8 startup_commands[] = {
    TUYA_CONNECTION_STATE, TUYA_HEARTBEAT, TUYA_MCU_INFO, TUYA_MCU_VERSION,
    TUYA_WORK_MODE, TUYA_CONNECTION_STATE, TUYA_QUERY_DPS,
};

static void startup_poll(void)
{
    const u8 disconnected = 1;
    const u32 delay = startup_step == 0 ? 1200000 : startup_step == 1 ? 2000000 : 80000;
    if (startup_step < NUM(startup_commands) && clock_time_exceed(startup_tick, delay)) {
        u8 command = startup_commands[startup_step];
        const u8 is_state = command == TUYA_CONNECTION_STATE;
        if (send_frame(command, is_state ? &disconnected : NULL, is_state)) {
            startup_tick = clock_time();
            ++startup_step;
        }
    }
    if (command_pending && clock_time_exceed(command_tick, 2500000)) {
        if (send_frame(TUYA_QUERY_DPS, NULL, 0)) {
            command_pending = 0;
            valve_known = 0;
        }
    }
}

static u8 uart_idle_for_sleep(void)
{
    return low_power_enabled && module_wake_verified &&
           startup_step == NUM(startup_commands) &&
           !command_pending && !join_requested &&
           !mcu_wake_active && tx_in == tx_out &&
           rx_in == rx_out && !rx_overflow && !parser.used &&
           !gpio_read(MODULE_WAKE_PIN) && !uart_tx_is_busy() &&
           clock_time_exceed(last_rx_tick, UART_IDLE_US) &&
           clock_time_exceed(module_wake_tick, UART_IDLE_US);
}

static void suspend_when_idle(void)
{
    if (BDB_STATE_GET() != BDB_STATE_IDLE || !uart_idle_for_sleep() ||
        tl_stackBusy() || !zb_isTaskDone()) {
        return;
    }
    apsCleanToStopSecondClock();
    const u8 irq = irq_disable();
    // Recheck after masking IRQs. A subsequent rising PB5 level wakes the chip.
    if (uart_idle_for_sleep()) {
        const ev_timer_event_t *timer = ev_timer_nearestGet();
        const u32 delay = timer && timer->timeout < POLL_INTERVAL_MS ? timer->timeout : POLL_INTERVAL_MS;
        if (delay >= 5) {
            drv_pm_sleep(PM_SLEEP_MODE_SUSPEND, PM_WAKEUP_SRC_PAD | PM_WAKEUP_SRC_TIMER, delay);
        }
    }
    irq_restore(irq);
}

static void application_init(void)
{
    gpio_set_func(MCU_WAKE_PIN, AS_GPIO);
    gpio_write(MCU_WAKE_PIN, 0);
    gpio_set_output_en(MCU_WAKE_PIN, 1);
    gpio_set_func(MODULE_WAKE_PIN, AS_GPIO);
    gpio_set_output_en(MODULE_WAKE_PIN, 0);
    gpio_set_input_en(MODULE_WAKE_PIN, 1);
    gpio_setup_up_down_resistor(MODULE_WAKE_PIN, PM_PIN_PULLDOWN_100K);
    drv_pm_wakeupPinConfig(&wake_pin, 1);
    reg_clk_en0 |= FLD_CLK0_UART_EN;
    drv_uart_pin_set(UART_TX_PB1, UART_RX_PB7);
    drv_uart_init(9600, (u8 *)rx_dma, sizeof(rx_dma), rx_irq);
    zb_init();
    zb_zdoCbRegister(&zdo_callbacks);
    zcl_init(zcl_hook);
    af_endpointRegister(EP, &descriptor, zcl_rx_handler, NULL);
    af_powerDescPowerModeUpdate(POWER_MODE_RECEIVER_COMES_PERIODICALLY);
    zcl_reportingTabInit();
    zcl_basic_register(EP, MANUFACTURER_CODE_NONE, NUM(basic_attrs), basic_attrs, NULL);
    zcl_identify_register(EP, MANUFACTURER_CODE_NONE, NUM(identify_attrs), identify_attrs, NULL);
    zcl_registerCluster(EP, ZCL_CLUSTER_GEN_ON_OFF, MANUFACTURER_CODE_NONE,
                        NUM(onoff_attrs), onoff_attrs, onoff_command, NULL);
    zcl_powerCfg_register(EP, MANUFACTURER_CODE_NONE, NUM(battery_attrs), battery_attrs, NULL);
    zcl_registerCluster(EP, GARDEN_CONTROL_CLUSTER, MANUFACTURER_CODE_NONE,
                        0, NULL, garden_control_command, NULL);
    u8 change = 0;
    bdb_defaultReportingCfg(EP, HA_PROFILE_ID, ZCL_CLUSTER_GEN_ON_OFF, ZCL_ATTRID_ONOFF, 0, 300, &change);
    change = 2;
    bdb_defaultReportingCfg(EP, HA_PROFILE_ID, ZCL_CLUSTER_GEN_POWER_CFG,
                            ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING, 10, 3600, &change);
    bdb_init(&descriptor, &commission, &bdb_callbacks, 1);
}

int main(void)
{
    drv_platform_init();
    os_init(0);
    startup_tick = clock_time();
    application_init();
    irq_enable();
    while (1) {
        uart_poll();
        ev_main();
        tl_zbTaskProcedure();
        startup_poll();
        if (BDB_STATE_GET() == BDB_STATE_IDLE) {
            if (join_requested || (rejoin_pending && clock_time_exceed(rejoin_tick, 10000000))) {
                join_requested = 0;
                rejoin_pending = 0;
                bdb_networkSteerStart();
            }
            if (zb_isDeviceJoinedNwk()) {
                report_handler();
            }
        }
        suspend_when_idle();
    }
}
