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

LOG_MODULE_REGISTER(uart_9p, CONFIG_LOG_DEFAULT_LEVEL);

/* Ring buffer for keystrokes (2 bytes per event: scancode + flags) */
RING_BUF_DECLARE(key_ring, CONFIG_UART_9P_KEY_RING_SIZE * 2);

/* Current LED state */
static uint8_t led_state;

/*
 * HID to PS/2 Set 1 Scan Code Translation
 */
static const uint8_t hid_to_ps2_table[] = {
    [0x04] = 0x1E,  /* A */
    [0x05] = 0x30,  /* B */
    [0x06] = 0x2E,  /* C */
    [0x07] = 0x20,  /* D */
    [0x08] = 0x12,  /* E */
    [0x09] = 0x21,  /* F */
    [0x0A] = 0x22,  /* G */
    [0x0B] = 0x23,  /* H */
    [0x0C] = 0x17,  /* I */
    [0x0D] = 0x24,  /* J */
    [0x0E] = 0x25,  /* K */
    [0x0F] = 0x26,  /* L */
    [0x10] = 0x32,  /* M */
    [0x11] = 0x31,  /* N */
    [0x12] = 0x18,  /* O */
    [0x13] = 0x19,  /* P */
    [0x14] = 0x10,  /* Q */
    [0x15] = 0x13,  /* R */
    [0x16] = 0x1F,  /* S */
    [0x17] = 0x14,  /* T */
    [0x18] = 0x16,  /* U */
    [0x19] = 0x2F,  /* V */
    [0x1A] = 0x11,  /* W */
    [0x1B] = 0x2D,  /* X */
    [0x1C] = 0x15,  /* Y */
    [0x1D] = 0x2C,  /* Z */
    [0x1E] = 0x02,  /* 1 */
    [0x1F] = 0x03,  /* 2 */
    [0x20] = 0x04,  /* 3 */
    [0x21] = 0x05,  /* 4 */
    [0x22] = 0x06,  /* 5 */
    [0x23] = 0x07,  /* 6 */
    [0x24] = 0x08,  /* 7 */
    [0x25] = 0x09,  /* 8 */
    [0x26] = 0x0A,  /* 9 */
    [0x27] = 0x0B,  /* 0 */
    [0x28] = 0x1C,  /* Enter */
    [0x29] = 0x01,  /* Escape */
    [0x2A] = 0x0E,  /* Backspace */
    [0x2B] = 0x0F,  /* Tab */
    [0x2C] = 0x39,  /* Space */
    [0x2D] = 0x0C,  /* - _ */
    [0x2E] = 0x0D,  /* = + */
    [0x2F] = 0x1A,  /* [ { */
    [0x30] = 0x1B,  /* ] } */
    [0x31] = 0x2B,  /* \ | */
    [0x33] = 0x27,  /* ; : */
    [0x34] = 0x28,  /* ' " */
    [0x35] = 0x29,  /* ` ~ */
    [0x36] = 0x33,  /* , < */
    [0x37] = 0x34,  /* . > */
    [0x38] = 0x35,  /* / ? */
    [0x39] = 0x3A,  /* Caps Lock */
    [0x3A] = 0x3B,  /* F1 */
    [0x3B] = 0x3C,  /* F2 */
    [0x3C] = 0x3D,  /* F3 */
    [0x3D] = 0x3E,  /* F4 */
    [0x3E] = 0x3F,  /* F5 */
    [0x3F] = 0x40,  /* F6 */
    [0x40] = 0x41,  /* F7 */
    [0x41] = 0x42,  /* F8 */
    [0x42] = 0x43,  /* F9 */
    [0x43] = 0x44,  /* F10 */
    [0x44] = 0x57,  /* F11 */
    [0x45] = 0x58,  /* F12 */
    /* Modifiers */
    [0xE0] = 0x1D,  /* Left Control */
    [0xE1] = 0x2A,  /* Left Shift */
    [0xE2] = 0x38,  /* Left Alt */
    [0xE3] = 0x5B,  /* Left GUI */
    [0xE4] = 0x1D,  /* Right Control (extended) */
    [0xE5] = 0x36,  /* Right Shift */
    [0xE6] = 0x38,  /* Right Alt (extended) */
    [0xE7] = 0x5C,  /* Right GUI (extended) */
};

static inline uint8_t hid_to_ps2(uint16_t hid)
{
    if (hid < sizeof(hid_to_ps2_table)) {
        return hid_to_ps2_table[hid];
    }
    return 0;
}

static inline bool is_extended_key(uint16_t hid)
{
    /* Right-side modifiers need E0 prefix in PS/2 */
    return (hid >= 0xE4 && hid <= 0xE7);
}

/*
 * Queue a keystroke for 9P clients to read.
 */
static void queue_keystroke(uint8_t scancode, uint8_t flags)
{
    uint8_t data[2] = { scancode, flags };
    uint32_t written = ring_buf_put(&key_ring, data, 2);
    if (written != 2) {
        LOG_WRN("Key ring buffer full, dropping keystroke");
    }
}

/*
 * ZMK key event listener - converts HID to PS/2 and queues.
 */
static int key_listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t scancode = hid_to_ps2(ev->keycode);
    if (scancode == 0) {
        return ZMK_EV_EVENT_BUBBLE;  /* Unknown key */
    }

    uint8_t flags = ev->state ? 0x00 : 0x01;  /* press=0, release=1 */
    if (is_extended_key(ev->keycode)) {
        flags |= 0x80;  /* Extended key flag */
    }

    queue_keystroke(scancode, flags);
    LOG_DBG("Key %s: HID=0x%02X PS2=0x%02X flags=0x%02X",
            ev->state ? "press" : "release", ev->keycode, scancode, flags);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(uart_9p_server, key_listener);
ZMK_SUBSCRIPTION(uart_9p_server, zmk_keycode_state_changed);

/*
 * 9P Filesystem Implementation
 */

/* File nodes */
static struct ninep_fs_node kbd_root_node = {
    .name = "",
    .type = NINEP_NODE_DIR,
    .qid = { .type = NINEP_QTDIR, .version = 0, .path = 1 }
};

static struct ninep_fs_node kbin_node = {
    .name = "kbin",
    .type = NINEP_NODE_FILE,
    .qid = { .type = NINEP_QTFILE, .version = 0, .path = 2 }
};

static struct ninep_fs_node leds_node = {
    .name = "leds",
    .type = NINEP_NODE_FILE,
    .qid = { .type = NINEP_QTFILE, .version = 0, .path = 3 }
};

static struct ninep_fs_node *fs_get_root(void *ctx)
{
    ARG_UNUSED(ctx);
    return &kbd_root_node;
}

static struct ninep_fs_node *fs_walk(struct ninep_fs_node *parent,
                                      const char *name, uint16_t name_len,
                                      void *ctx)
{
    ARG_UNUSED(ctx);

    if (parent != &kbd_root_node) {
        return NULL;  /* Only root has children */
    }

    if (name_len == 4 && strncmp(name, "kbin", 4) == 0) {
        return &kbin_node;
    }
    if (name_len == 4 && strncmp(name, "leds", 4) == 0) {
        return &leds_node;
    }

    return NULL;
}

static int fs_open(struct ninep_fs_node *node, uint8_t mode, void *ctx)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(mode);
    ARG_UNUSED(node);
    return 0;  /* Always allow open */
}

static int fs_read(struct ninep_fs_node *node, uint64_t offset,
                   uint8_t *buf, uint32_t count, const char *uname, void *ctx)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(uname);
    ARG_UNUSED(offset);

    if (node == &kbin_node) {
        /* Read keystrokes from ring buffer */
        uint32_t read = ring_buf_get(&key_ring, buf, count);
        return (int)read;
    }

    if (node == &leds_node) {
        /* Return current LED state */
        if (count >= 1) {
            buf[0] = led_state;
            return 1;
        }
        return 0;
    }

    if (node == &kbd_root_node) {
        /* Directory read - return stat entries for children */
        /* For simplicity, just return empty for now */
        return 0;
    }

    return -ENOENT;
}

static int fs_write(struct ninep_fs_node *node, uint64_t offset,
                    const uint8_t *buf, uint32_t count, const char *uname,
                    void *ctx)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(uname);
    ARG_UNUSED(offset);

    if (node == &leds_node && count >= 1) {
        led_state = buf[0];
        LOG_DBG("LED state set to 0x%02X", led_state);
        /* TODO: Actually control LEDs if keyboard has them */
        return 1;
    }

    return -EACCES;  /* Write not permitted */
}

static int fs_stat(struct ninep_fs_node *node, uint8_t *buf,
                   size_t buf_len, void *ctx)
{
    ARG_UNUSED(ctx);

    uint32_t mode;
    uint64_t length = 0;
    const char *name = node->name;

    if (node == &kbd_root_node) {
        mode = 0x80000000 | 0755;  /* Directory */
        name = "";
    } else if (node == &kbin_node) {
        mode = 0444;  /* Read-only */
        length = ring_buf_size_get(&key_ring);
    } else if (node == &leds_node) {
        mode = 0222;  /* Write-only */
        length = 1;
    } else {
        return -ENOENT;
    }

    size_t offset = 0;
    return ninep_write_stat(buf, buf_len, &offset, &node->qid, mode,
                            0, name, strlen(name), "", "", "");
}

static int fs_clunk(struct ninep_fs_node *node, void *ctx)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(node);
    return 0;
}

static const struct ninep_fs_ops kbd_fs_ops = {
    .get_root = fs_get_root,
    .walk = fs_walk,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .stat = fs_stat,
    .clunk = fs_clunk,
    .create = NULL,
    .remove = NULL,
};

/*
 * Server state
 */
static struct ninep_server kbd_server;
static struct ninep_transport uart_transport;
static uint8_t uart_rx_buf[CONFIG_UART_9P_RX_BUF_SIZE];

/*
 * Transport receive callback
 */
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

    /* Get UART device */
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    /* Initialize UART transport */
    struct ninep_transport_uart_config uart_config = {
        .uart_dev = uart_dev,
        .rx_buf = uart_rx_buf,
        .rx_buf_size = sizeof(uart_rx_buf),
    };

    ret = ninep_transport_uart_init(&uart_transport, &uart_config,
                                     uart_recv_cb, &kbd_server);
    if (ret < 0) {
        LOG_ERR("Failed to init UART transport: %d", ret);
        return ret;
    }

    /* Initialize 9P server */
    struct ninep_server_config server_config = {
        .fs_ops = &kbd_fs_ops,
        .fs_ctx = NULL,
        .max_message_size = CONFIG_UART_9P_RX_BUF_SIZE,
        .version = "9P2000",
        .auth_config = NULL,
    };

    ret = ninep_server_init(&kbd_server, &server_config, &uart_transport);
    if (ret < 0) {
        LOG_ERR("Failed to init 9P server: %d", ret);
        return ret;
    }

    /* Start server */
    ret = ninep_server_start(&kbd_server);
    if (ret < 0) {
        LOG_ERR("Failed to start 9P server: %d", ret);
        return ret;
    }

    LOG_INF("UART 9P keyboard server started on %s", uart_dev->name);
    return 0;
}

SYS_INIT(uart_9p_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
