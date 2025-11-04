/*
 * Copyright (c) 2025 ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P Keyboard Server Implementation
 * Exposes /kbd/kbin (PS/2 scan codes) and /kbd/leds (LED control)
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

LOG_MODULE_REGISTER(kbd_9p, CONFIG_ZMK_LOG_LEVEL);

/* PS/2 Set 1 Scan Code Translation Table */
/* Maps HID usage codes (page 0x07 - Keyboard) to PS/2 Set 1 scan codes */
static const uint8_t hid_to_ps2_table[] = {
	[0x00] = 0x00,  // Reserved (no event)
	[0x01] = 0x00,  // ErrorRollOver
	[0x02] = 0x00,  // POSTFail
	[0x03] = 0x00,  // ErrorUndefined
	[0x04] = 0x1E,  // a A
	[0x05] = 0x30,  // b B
	[0x06] = 0x2E,  // c C
	[0x07] = 0x20,  // d D
	[0x08] = 0x12,  // e E
	[0x09] = 0x21,  // f F
	[0x0A] = 0x22,  // g G
	[0x0B] = 0x23,  // h H
	[0x0C] = 0x17,  // i I
	[0x0D] = 0x24,  // j J
	[0x0E] = 0x25,  // k K
	[0x0F] = 0x26,  // l L
	[0x10] = 0x32,  // m M
	[0x11] = 0x31,  // n N
	[0x12] = 0x18,  // o O
	[0x13] = 0x19,  // p P
	[0x14] = 0x10,  // q Q
	[0x15] = 0x13,  // r R
	[0x16] = 0x1F,  // s S
	[0x17] = 0x14,  // t T
	[0x18] = 0x16,  // u U
	[0x19] = 0x2F,  // v V
	[0x1A] = 0x11,  // w W
	[0x1B] = 0x2D,  // x X
	[0x1C] = 0x15,  // y Y
	[0x1D] = 0x2C,  // z Z
	[0x1E] = 0x02,  // 1 !
	[0x1F] = 0x03,  // 2 @
	[0x20] = 0x04,  // 3 #
	[0x21] = 0x05,  // 4 $
	[0x22] = 0x06,  // 5 %
	[0x23] = 0x07,  // 6 ^
	[0x24] = 0x08,  // 7 &
	[0x25] = 0x09,  // 8 *
	[0x26] = 0x0A,  // 9 (
	[0x27] = 0x0B,  // 0 )
	[0x28] = 0x1C,  // Enter
	[0x29] = 0x01,  // Escape
	[0x2A] = 0x0E,  // Backspace
	[0x2B] = 0x0F,  // Tab
	[0x2C] = 0x39,  // Space
	[0x2D] = 0x0C,  // - _
	[0x2E] = 0x0D,  // = +
	[0x2F] = 0x1A,  // [ {
	[0x30] = 0x1B,  // ] }
	[0x31] = 0x2B,  // \ |
	[0x32] = 0x2B,  // Non-US # ~
	[0x33] = 0x27,  // ; :
	[0x34] = 0x28,  // ' "
	[0x35] = 0x29,  // ` ~
	[0x36] = 0x33,  // , <
	[0x37] = 0x34,  // . >
	[0x38] = 0x35,  // / ?
	[0x39] = 0x3A,  // Caps Lock
	[0x3A] = 0x3B,  // F1
	[0x3B] = 0x3C,  // F2
	[0x3C] = 0x3D,  // F3
	[0x3D] = 0x3E,  // F4
	[0x3E] = 0x3F,  // F5
	[0x3F] = 0x40,  // F6
	[0x40] = 0x41,  // F7
	[0x41] = 0x42,  // F8
	[0x42] = 0x43,  // F9
	[0x43] = 0x44,  // F10
	[0x44] = 0x57,  // F11
	[0x45] = 0x58,  // F12
	[0x46] = 0x00,  // PrintScreen (extended: E0 2A E0 37)
	[0x47] = 0x46,  // Scroll Lock
	[0x48] = 0x00,  // Pause (special: E1 1D 45 E1 9D C5)
	[0x49] = 0x00,  // Insert (extended: E0 52)
	[0x4A] = 0x00,  // Home (extended: E0 47)
	[0x4B] = 0x00,  // Page Up (extended: E0 49)
	[0x4C] = 0x00,  // Delete (extended: E0 53)
	[0x4D] = 0x00,  // End (extended: E0 4F)
	[0x4E] = 0x00,  // Page Down (extended: E0 51)
	[0x4F] = 0x00,  // Right Arrow (extended: E0 4D)
	[0x50] = 0x00,  // Left Arrow (extended: E0 4B)
	[0x51] = 0x00,  // Down Arrow (extended: E0 50)
	[0x52] = 0x00,  // Up Arrow (extended: E0 48)
	[0x53] = 0x45,  // Num Lock
	[0x54] = 0x00,  // Keypad / (extended: E0 35)
	[0x55] = 0x37,  // Keypad *
	[0x56] = 0x4A,  // Keypad -
	[0x57] = 0x4E,  // Keypad +
	[0x58] = 0x00,  // Keypad Enter (extended: E0 1C)
	[0x59] = 0x4F,  // Keypad 1 End
	[0x5A] = 0x50,  // Keypad 2 Down
	[0x5B] = 0x51,  // Keypad 3 PageDn
	[0x5C] = 0x4B,  // Keypad 4 Left
	[0x5D] = 0x4C,  // Keypad 5
	[0x5E] = 0x4D,  // Keypad 6 Right
	[0x5F] = 0x47,  // Keypad 7 Home
	[0x60] = 0x48,  // Keypad 8 Up
	[0x61] = 0x49,  // Keypad 9 PageUp
	[0x62] = 0x52,  // Keypad 0 Insert
	[0x63] = 0x53,  // Keypad . Delete
	[0x64] = 0x56,  // Non-US \ |
	[0x65] = 0x00,  // Application (extended: E0 5D)
};

/* Extended keys that need E0 prefix */
struct extended_key {
	uint8_t hid_usage;
	uint8_t ps2_code;
};

static const struct extended_key extended_keys[] = {
	{0x49, 0x52},  // Insert
	{0x4A, 0x47},  // Home
	{0x4B, 0x49},  // Page Up
	{0x4C, 0x53},  // Delete
	{0x4D, 0x4F},  // End
	{0x4E, 0x51},  // Page Down
	{0x4F, 0x4D},  // Right Arrow
	{0x50, 0x4B},  // Left Arrow
	{0x51, 0x50},  // Down Arrow
	{0x52, 0x48},  // Up Arrow
	{0x54, 0x35},  // Keypad /
	{0x58, 0x1C},  // Keypad Enter
	{0x65, 0x5D},  // Application
	{0xE0, 0x1D},  // Left Control (extended)
	{0xE1, 0x2A},  // Left Shift
	{0xE2, 0x38},  // Left Alt
	{0xE3, 0x5B},  // Left GUI (extended)
	{0xE4, 0x1D},  // Right Control (extended)
	{0xE5, 0x36},  // Right Shift
	{0xE6, 0x38},  // Right Alt (extended)
	{0xE7, 0x5C},  // Right GUI (extended)
};

/* Modifier keys mapping */
static const uint8_t modifier_codes[] = {
	[0] = 0x1D,  // Left Control (extended for right: E0 1D)
	[1] = 0x2A,  // Left Shift
	[2] = 0x38,  // Left Alt (extended for right: E0 38)
	[3] = 0x5B,  // Left GUI (extended)
	[4] = 0x1D,  // Right Control (needs E0 prefix)
	[5] = 0x36,  // Right Shift
	[6] = 0x38,  // Right Alt (needs E0 prefix)
	[7] = 0x5C,  // Right GUI (needs E0 prefix)
};

/* Circular buffer for scan codes */
#define SCANCODE_BUF_SIZE 128
static uint8_t scancode_buf[SCANCODE_BUF_SIZE];
static size_t scancode_head = 0;
static size_t scancode_tail = 0;
static K_MUTEX_DEFINE(scancode_mutex);

/* LED state */
static uint8_t led_state = 0;
static K_MUTEX_DEFINE(led_mutex);

/* Helper: Add scan code to buffer */
static void add_scancode(uint8_t code)
{
	k_mutex_lock(&scancode_mutex, K_FOREVER);
	scancode_buf[scancode_head] = code;
	scancode_head = (scancode_head + 1) % SCANCODE_BUF_SIZE;

	/* If buffer full, drop oldest */
	if (scancode_head == scancode_tail) {
		scancode_tail = (scancode_tail + 1) % SCANCODE_BUF_SIZE;
	}
	k_mutex_unlock(&scancode_mutex);
}

/* Helper: Translate HID usage to PS/2 scan code */
static void translate_hid_to_ps2(uint32_t hid_usage, bool pressed)
{
	uint8_t usage_id = hid_usage & 0xFF;

	/* Check if it's a modifier (0xE0-0xE7) */
	if (usage_id >= 0xE0 && usage_id <= 0xE7) {
		uint8_t mod_idx = usage_id - 0xE0;
		uint8_t code = modifier_codes[mod_idx];

		/* Right modifiers (4-7) need E0 prefix except Right Shift */
		if (mod_idx >= 4 && mod_idx != 5) {
			add_scancode(0xE0);
		}

		add_scancode(pressed ? code : (code | 0x80));
		LOG_DBG("Modifier 0x%02X -> PS/2 0x%02X %s", usage_id, code,
		        pressed ? "press" : "release");
		return;
	}

	/* Check extended keys table */
	for (int i = 0; i < ARRAY_SIZE(extended_keys); i++) {
		if (extended_keys[i].hid_usage == usage_id) {
			add_scancode(0xE0);
			add_scancode(pressed ? extended_keys[i].ps2_code :
			             (extended_keys[i].ps2_code | 0x80));
			LOG_DBG("Extended 0x%02X -> PS/2 E0 0x%02X %s", usage_id,
			        extended_keys[i].ps2_code, pressed ? "press" : "release");
			return;
		}
	}

	/* Look up in main table */
	if (usage_id < ARRAY_SIZE(hid_to_ps2_table)) {
		uint8_t code = hid_to_ps2_table[usage_id];
		if (code != 0x00) {
			add_scancode(pressed ? code : (code | 0x80));
			LOG_DBG("HID 0x%02X -> PS/2 0x%02X %s", usage_id, code,
			        pressed ? "press" : "release");
			return;
		}
	}

	LOG_WRN("Unmapped HID usage: 0x%02X", usage_id);
}

/* ZMK event listener for keycode state changes */
static int keycode_event_listener(const zmk_event_t *eh)
{
	const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
	if (!ev) {
		return 0;
	}

	/* Only handle keyboard page (0x07) */
	if (ev->usage_page != HID_USAGE_KEY) {
		return 0;
	}

	LOG_DBG("Key event: usage=0x%04X state=%d", ev->keycode, ev->state);
	translate_hid_to_ps2(ev->keycode, ev->state);

	return 0;
}

ZMK_LISTENER(kbd_9p_keycode, keycode_event_listener);
ZMK_SUBSCRIPTION(kbd_9p_keycode, zmk_keycode_state_changed);

/* 9P Filesystem nodes */
static struct ninep_fs_node kbd_root_node;
static struct ninep_fs_node kbin_node;
static struct ninep_fs_node leds_node;

/* Filesystem operations */
static struct ninep_fs_node *fs_get_root(void *ctx)
{
	return &kbd_root_node;
}

static struct ninep_fs_node *fs_walk(struct ninep_fs_node *parent,
                                      const char *name, uint16_t name_len,
                                      void *ctx)
{
	if (parent != &kbd_root_node) {
		return NULL;
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
	/* Allow any mode for now */
	return 0;
}

static int fs_read(struct ninep_fs_node *node, uint64_t offset,
                   uint8_t *buf, uint32_t count, void *ctx)
{
	if (node == &kbin_node) {
		/* Read scan codes from buffer */
		k_mutex_lock(&scancode_mutex, K_FOREVER);

		size_t available = 0;
		if (scancode_head >= scancode_tail) {
			available = scancode_head - scancode_tail;
		} else {
			available = SCANCODE_BUF_SIZE - scancode_tail + scancode_head;
		}

		size_t to_read = MIN(count, available);
		size_t read_count = 0;

		while (read_count < to_read && scancode_tail != scancode_head) {
			buf[read_count++] = scancode_buf[scancode_tail];
			scancode_tail = (scancode_tail + 1) % SCANCODE_BUF_SIZE;
		}

		k_mutex_unlock(&scancode_mutex);

		LOG_DBG("Read %zu scan codes from kbin", read_count);
		return read_count;
	}

	if (node == &leds_node) {
		/* Read current LED state */
		if (count > 0) {
			k_mutex_lock(&led_mutex, K_FOREVER);
			buf[0] = led_state;
			k_mutex_unlock(&led_mutex);
			LOG_DBG("Read LED state: 0x%02X", buf[0]);
			return 1;
		}
		return 0;
	}

	return -EINVAL;
}

static int fs_write(struct ninep_fs_node *node, uint64_t offset,
                    const uint8_t *buf, uint32_t count, void *ctx)
{
	if (node == &leds_node) {
		/* Write LED state */
		if (count > 0) {
			k_mutex_lock(&led_mutex, K_FOREVER);
			led_state = buf[0];
			k_mutex_unlock(&led_mutex);

			LOG_INF("LED state updated: 0x%02X (NumLock=%d CapsLock=%d ScrollLock=%d)",
			        led_state,
			        !!(led_state & 0x01),
			        !!(led_state & 0x02),
			        !!(led_state & 0x04));

			/* TODO: Actually update keyboard LEDs via ZMK */

			return 1;
		}
		return 0;
	}

	/* kbin is read-only */
	return -EPERM;
}

static int fs_stat(struct ninep_fs_node *node, uint8_t *buf, size_t buf_size,
                   void *ctx)
{
	/* Use ninep_write_stat to encode stat information */
	size_t offset = 0;
	const char *name, *uid, *gid, *muid;
	uint32_t mode;
	uint64_t length;

	if (node == &kbd_root_node) {
		name = "kbd";
		mode = 0x80000000 | 0755;  // Directory with rwxr-xr-x
		length = 0;
	} else if (node == &kbin_node) {
		name = "kbin";
		mode = 0444;  // Read-only file
		length = 0xFFFFFFFFFFFFFFFFULL;  // Infinite stream
	} else if (node == &leds_node) {
		name = "leds";
		mode = 0644;  // Read-write file
		length = 1;
	} else {
		return -EINVAL;
	}

	uid = "zmk";
	gid = "zmk";
	muid = "zmk";

	/* Write stat using ninep_write_stat helper */
	int ret = ninep_write_stat(buf, buf_size, &offset, &node->qid, mode,
	                            length, name, strlen(name));
	if (ret < 0) {
		return ret;
	}

	/* ninep_write_stat returns offset, we need to return total bytes written */
	return offset;
}

static const struct ninep_fs_ops kbd_fs_ops = {
	.get_root = fs_get_root,
	.walk = fs_walk,
	.open = fs_open,
	.read = fs_read,
	.write = fs_write,
	.stat = fs_stat,
	.create = NULL,
	.remove = NULL,
};

/* 9P server and transport */
static struct ninep_server kbd_server;
static struct ninep_transport kbd_transport;
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* BLE advertising */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x01, 0x10),  // Custom 9P service (0x1001 little-endian)
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("BLE connection failed (err %u)", err);
		return;
	}
	LOG_INF("BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("BLE disconnected (reason %u)", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Initialize 9P keyboard server */
int kbd_9p_server_init(void)
{
	int ret;

	/* Initialize filesystem nodes */
	kbd_root_node = (struct ninep_fs_node){
		.qid = {.type = NINEP_QTDIR, .version = 0, .path = 1},
	};
	kbin_node = (struct ninep_fs_node){
		.qid = {.type = NINEP_QTFILE, .version = 0, .path = 2},
	};
	leds_node = (struct ninep_fs_node){
		.qid = {.type = NINEP_QTFILE, .version = 0, .path = 3},
	};

	/* Initialize BLE */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return ret;
	}
	LOG_INF("Bluetooth initialized");

	/* Initialize L2CAP transport */
	struct ninep_transport_l2cap_config l2cap_config = {
		.psm = 0x1001,
		.rx_buf = rx_buf,
		.rx_buf_size = sizeof(rx_buf),
	};

	ret = ninep_transport_l2cap_init(&kbd_transport, &l2cap_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to init L2CAP transport: %d", ret);
		return ret;
	}
	LOG_INF("L2CAP transport initialized");

	/* Initialize 9P server */
	struct ninep_server_config server_config = {
		.fs_ops = &kbd_fs_ops,
		.fs_ctx = NULL,
	};

	ret = ninep_server_init(&kbd_server, &server_config, &kbd_transport);
	if (ret < 0) {
		LOG_ERR("Failed to init 9P server: %d", ret);
		return ret;
	}
	LOG_INF("9P server initialized");

	/* Start 9P server */
	ret = ninep_server_start(&kbd_server);
	if (ret < 0) {
		LOG_ERR("Failed to start 9P server: %d", ret);
		return ret;
	}
	LOG_INF("9P server started");

	/* Start BLE advertising */
	ret = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("Advertising failed to start (err %d)", ret);
		return ret;
	}
	LOG_INF("BLE advertising started");

	return 0;
}

/* Initialize at application level */
SYS_INIT(kbd_9p_server_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
