/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 *
 * 9P Keyboard Server over UART
 *
 * Exposes keyboard as /kbd namespace over UART for wired connection
 * to FRST terminal (ESP32-S3).
 *
 * Namespace:
 *   /kbd/kbin  - Keystroke stream (read-only, PS/2 scan codes)
 *   /kbd/leds  - LED indicator state (write-only)
 *
 * Non-blocking reads: the deck polls kbin every 50ms.
 * No worker thread needed — ISR delivers messages directly to
 * the server via the transport's recv_cb → process_message path.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/message.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include <string.h>
#include <version.h>

LOG_MODULE_REGISTER(uart_9p, CONFIG_LOG_DEFAULT_LEVEL);

#define FRST_FW_VERSION "0.3.0-dev+" STRINGIFY(BUILD_VERSION)

/* Scancode ring buffer — raw PS/2 byte stream */
RING_BUF_DECLARE(key_ring, CONFIG_UART_9P_KEY_RING_SIZE * 2);

/* Current LED state */
static uint8_t led_state;

/*
 * HID to PS/2 Set 1 scan code table
 */
static const uint8_t hid_to_ps2_table[] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,
    [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F,
    [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,
    [0x30] = 0x1B, [0x31] = 0x2B, [0x33] = 0x27, [0x34] = 0x28,
    [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35,
    [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
};

static const struct { uint8_t hid; uint8_t ps2; } extended_keys[] = {
    {0x49, 0x52}, {0x4A, 0x47}, {0x4B, 0x49}, {0x4C, 0x53},
    {0x4D, 0x4F}, {0x4E, 0x51}, {0x4F, 0x4D}, {0x50, 0x4B},
    {0x51, 0x50}, {0x52, 0x48}, {0x54, 0x35}, {0x58, 0x1C},
    {0x65, 0x5D},
};

static const uint8_t modifier_codes[] = {
    0x1D, 0x2A, 0x38, 0x5B, 0x1D, 0x36, 0x38, 0x5C,
};

static void add_scancode(uint8_t code)
{
    if (ring_buf_put(&key_ring, &code, 1) != 1) {
        LOG_WRN("Scancode buffer full");
    }
}

static void translate_hid_to_ps2(uint32_t hid_usage, bool pressed)
{
    uint8_t usage_id = hid_usage & 0xFF;

    if (usage_id >= 0xE0 && usage_id <= 0xE7) {
        uint8_t mod_idx = usage_id - 0xE0;
        uint8_t code = modifier_codes[mod_idx];
        if (mod_idx >= 4 && mod_idx != 5) add_scancode(0xE0);
        add_scancode(pressed ? code : (code | 0x80));
        return;
    }

    for (int i = 0; i < ARRAY_SIZE(extended_keys); i++) {
        if (extended_keys[i].hid == usage_id) {
            add_scancode(0xE0);
            add_scancode(pressed ? extended_keys[i].ps2 :
                         (extended_keys[i].ps2 | 0x80));
            return;
        }
    }

    if (usage_id < ARRAY_SIZE(hid_to_ps2_table)) {
        uint8_t code = hid_to_ps2_table[usage_id];
        if (code != 0) {
            add_scancode(pressed ? code : (code | 0x80));
        }
    }
}

static int key_listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) return ZMK_EV_EVENT_BUBBLE;
    translate_hid_to_ps2(ev->keycode, ev->state);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(uart_9p_server, key_listener);
ZMK_SUBSCRIPTION(uart_9p_server, zmk_keycode_state_changed);

/*
 * 9P Filesystem
 */
static struct ninep_fs_node kbd_root_node = {
    .name = "", .type = NINEP_NODE_DIR,
    .qid = { .type = NINEP_QTDIR, .version = 0, .path = 1 }
};
static struct ninep_fs_node kbin_node = {
    .name = "kbin", .type = NINEP_NODE_FILE,
    .qid = { .type = NINEP_QTFILE, .version = 0, .path = 2 }
};
static struct ninep_fs_node leds_node = {
    .name = "leds", .type = NINEP_NODE_FILE,
    .qid = { .type = NINEP_QTFILE, .version = 0, .path = 3 }
};

static struct ninep_fs_node *fs_get_root(void *ctx)
    { ARG_UNUSED(ctx); return &kbd_root_node; }

static struct ninep_fs_node *fs_walk(struct ninep_fs_node *parent,
    const char *name, uint16_t name_len, void *ctx)
{
    ARG_UNUSED(ctx);
    if (parent != &kbd_root_node) return NULL;
    if (name_len == 4 && strncmp(name, "kbin", 4) == 0) return &kbin_node;
    if (name_len == 4 && strncmp(name, "leds", 4) == 0) return &leds_node;
    return NULL;
}

static int fs_open(struct ninep_fs_node *node, uint8_t mode, void *ctx)
    { ARG_UNUSED(ctx); ARG_UNUSED(mode); ARG_UNUSED(node); return 0; }

static int fs_read(struct ninep_fs_node *node, uint64_t offset,
    uint8_t *buf, uint32_t count, const char *uname, void *ctx)
{
    ARG_UNUSED(ctx); ARG_UNUSED(uname); ARG_UNUSED(offset);

    if (node == &kbin_node) {
        return (int)ring_buf_get(&key_ring, buf, count);
    }
    if (node == &leds_node) {
        if (count >= 1) { buf[0] = led_state; return 1; }
        return 0;
    }
    if (node == &kbd_root_node) return 0;
    return -ENOENT;
}

static int fs_write(struct ninep_fs_node *node, uint64_t offset,
    const uint8_t *buf, uint32_t count, const char *uname, void *ctx)
{
    ARG_UNUSED(ctx); ARG_UNUSED(uname); ARG_UNUSED(offset);
    if (node == &leds_node && count >= 1) { led_state = buf[0]; return 1; }
    return -EACCES;
}

static int fs_stat(struct ninep_fs_node *node, uint8_t *buf,
    size_t buf_len, void *ctx)
{
    ARG_UNUSED(ctx);
    uint32_t mode; uint64_t length = 0;
    const char *name = node->name;
    if (node == &kbd_root_node) { mode = 0x80000000 | 0755; name = ""; }
    else if (node == &kbin_node) { mode = 0444; length = ring_buf_size_get(&key_ring); }
    else if (node == &leds_node) { mode = 0222; length = 1; }
    else return -ENOENT;
    size_t off = 0;
    return ninep_write_stat(buf, buf_len, &off, &node->qid, mode,
                            0, name, strlen(name), "", "", "");
}

static int fs_clunk(struct ninep_fs_node *node, void *ctx)
    { ARG_UNUSED(ctx); ARG_UNUSED(node); return 0; }

static const struct ninep_fs_ops kbd_fs_ops = {
    .get_root = fs_get_root, .walk = fs_walk, .open = fs_open,
    .read = fs_read, .write = fs_write, .stat = fs_stat,
    .clunk = fs_clunk, .create = NULL, .remove = NULL,
};

/*
 * Server state — no worker thread needed for non-blocking reads.
 * The UART ISR calls recv_cb which calls process_message directly.
 */
static struct ninep_server kbd_server;
static struct ninep_transport uart_transport;
static uint8_t uart_rx_buf[CONFIG_UART_9P_RX_BUF_SIZE];

static void uart_recv_cb(struct ninep_transport *transport,
                          const uint8_t *data, size_t len, void *user_data)
{
    struct ninep_server *server = user_data;
    ninep_server_process_message(server, data, len);
}

/*
 * Initialize UART 9P server
 */
static int uart_9p_init(void)
{
    int ret;

    LOG_INF("FRST Keyboard Firmware %s", FRST_FW_VERSION);

    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("[9P] UART0 device not ready");
        return -ENODEV;
    }

    /* Drain boot noise */
    { uint8_t c; while (uart_poll_in(uart_dev, &c) == 0) {} }

    struct ninep_transport_uart_config uart_config = {
        .uart_dev = uart_dev,
        .rx_buf = uart_rx_buf,
        .rx_buf_size = sizeof(uart_rx_buf),
    };

    ret = ninep_transport_uart_init(&uart_transport, &uart_config,
                                     uart_recv_cb, &kbd_server);
    if (ret < 0) { LOG_ERR("[9P] Transport init failed: %d", ret); return ret; }

    struct ninep_server_config server_config = {
        .fs_ops = &kbd_fs_ops,
        .fs_ctx = NULL,
        .max_message_size = CONFIG_UART_9P_RX_BUF_SIZE,
        .version = "9P2000",
        .auth_config = NULL,
    };

    ret = ninep_server_init(&kbd_server, &server_config, &uart_transport);
    if (ret < 0) { LOG_ERR("[9P] Server init failed: %d", ret); return ret; }

    ret = ninep_server_start(&kbd_server);
    if (ret < 0) { LOG_ERR("[9P] Server start failed: %d", ret); return ret; }

    LOG_INF("[9P] Server ready on %s (GP0/GP1, %d baud)",
            uart_dev->name, DT_PROP(DT_NODELABEL(uart0), current_speed));

    return 0;
}

SYS_INIT(uart_9p_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
