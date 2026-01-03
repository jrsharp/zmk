/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 *
 * 9P Keyboard Server Implementation
 * Exposes keyboard I/O, settings, and device management via 9P filesystem
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/battery.h>
#include <zephyr/drivers/sensor.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#if IS_ENABLED(CONFIG_NINEP_DFU)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/storage/flash_map.h>
#endif

#if IS_ENABLED(CONFIG_NINEP_GATT_9PIS)
#include <zephyr/9p/gatt_9pis.h>
#endif

#if IS_ENABLED(CONFIG_MEMFAULT)
#include <memfault/core/data_packetizer.h>
#endif

/* Version info from build system */
#include <app_version.h>
#include <version.h>
#include <zephyr/drivers/hwinfo.h>

/* Runtime settings APIs */
#include <zmk/activity.h>
#include <zmk/kscan_settings.h>
#include <frst_settings.h>

/* FRST firmware version - override with -DFRST_FW_VERSION at build time */
#ifndef FRST_FW_VERSION
#define FRST_FW_VERSION "0.1.0-beta.1"
#endif
#define FRST_PRODUCT "FRST-M2KB"

/* Use Zephyr's STRINGIFY macro from toolchain/common.h (included via kernel.h) */

LOG_MODULE_REGISTER(kbd_9p, CONFIG_ZMK_LOG_LEVEL);

/* Battery sensor device */
#if DT_HAS_CHOSEN(zmk_battery)
static const struct device *const battery_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
#else
static const struct device *const battery_dev = NULL;
#endif

/* Kscan device for debounce settings */
#if DT_HAS_CHOSEN(zmk_kscan)
static const struct device *const kscan_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_kscan));
#else
static const struct device *const kscan_dev = NULL;
#endif

/* Status LED - nice!nano blue LED on P0.15 */
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static bool status_led_ready = false;

static void status_led_set(bool on)
{
	if (status_led_ready) {
		gpio_pin_set_dt(&status_led, on ? 1 : 0);
	}
}

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

/* Semaphore for blocking reads on kbin - signaled when data available */
static K_SEM_DEFINE(kbin_data_sem, 0, 1);

/* Flag to indicate if a client is connected (for clean disconnect handling) */
static atomic_t kbin_client_connected = ATOMIC_INIT(0);

/* Timeout for blocking reads (30 seconds) - sends empty response for keepalive */
#define KBIN_READ_TIMEOUT_MS 30000

/* LED state */
static uint8_t led_state = 0;
static K_MUTEX_DEFINE(led_mutex);

/* DFU state */
#if IS_ENABLED(CONFIG_NINEP_DFU)
enum dfu_state {
	DFU_IDLE,
	DFU_ERASING,
	DFU_RECEIVING,
	DFU_FINALIZING,
	DFU_COMPLETE,
	DFU_ERROR,
};

static struct {
	enum dfu_state state;
	uint32_t bytes_written;
	int last_error;
	uint32_t last_progress_log;
	struct flash_img_context flash_ctx;
} dfu = {
	.state = DFU_IDLE,
};

#define DFU_PROGRESS_LOG_INTERVAL (50 * 1024)

static const char *dfu_state_names[] = {
	[DFU_IDLE] = "idle",
	[DFU_ERASING] = "erasing",
	[DFU_RECEIVING] = "receiving",
	[DFU_FINALIZING] = "finalizing",
	[DFU_COMPLETE] = "complete",
	[DFU_ERROR] = "error",
};
#endif /* CONFIG_NINEP_DFU */

/* Helper: Add scan code to buffer */
static void add_scancode(uint8_t code)
{
	k_mutex_lock(&scancode_mutex, K_FOREVER);
	scancode_buf[scancode_head] = code;
	scancode_head = (scancode_head + 1) % SCANCODE_BUF_SIZE;

	/* If buffer full, drop oldest */
	if (scancode_head == scancode_tail) {
		printk("[PS2] OVERFLOW dropping oldest!\n");
		scancode_tail = (scancode_tail + 1) % SCANCODE_BUF_SIZE;
	}
	k_mutex_unlock(&scancode_mutex);

	/* Debug: show scancode added and current state */
	printk("[PS2] +0x%02X (connected=%d, sem=%u)\n",
	       code,
	       (int)atomic_get(&kbin_client_connected),
	       k_sem_count_get(&kbin_data_sem));

	/* Wake up any blocking reader */
	k_sem_give(&kbin_data_sem);
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

	/* Direct console output for debugging - shows key detection independent of 9P */
	printk("[KEY] HID=0x%02X %s\n", ev->keycode, ev->state ? "DN" : "UP");

	translate_hid_to_ps2(ev->keycode, ev->state);

	return 0;
}

ZMK_LISTENER(kbd_9p_keycode, keycode_event_listener);
ZMK_SUBSCRIPTION(kbd_9p_keycode, zmk_keycode_state_changed);

/* Also listen to position events to diagnose kscan issues */
#include <zmk/events/position_state_changed.h>

static int position_event_listener(const zmk_event_t *eh)
{
	const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
	if (!ev) {
		return 0;
	}

	printk("[POS] row=%d col=%d %s\n",
	       ev->position / 10, ev->position % 10,
	       ev->state ? "DN" : "UP");
	return 0;
}

ZMK_LISTENER(kbd_9p_position, position_event_listener);
ZMK_SUBSCRIPTION(kbd_9p_position, zmk_position_state_changed);

/* 9P Filesystem nodes */
static struct ninep_fs_node kbd_root_node = {
	.qid = {.type = NINEP_QTDIR, .version = 0, .path = 1},
};
static struct ninep_fs_node kbin_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 2},
};
static struct ninep_fs_node leds_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 3},
};
static struct ninep_fs_node battery_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 4},
};

/* Control node for system commands (reboot, etc.) */
static struct ninep_fs_node ctl_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 5},
};

/* Version info node */
static struct ninep_fs_node version_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 9},
};

/* Settings directory and nodes */
static struct ninep_fs_node settings_node = {
	.qid = {.type = NINEP_QTDIR, .version = 0, .path = 10},
};
static struct ninep_fs_node debounce_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 11},
};
static struct ninep_fs_node idle_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 12},
};

/* /dev directory - exists when DFU or Memfault is enabled */
#if IS_ENABLED(CONFIG_NINEP_DFU) || IS_ENABLED(CONFIG_MEMFAULT)
static struct ninep_fs_node dev_node = {
	.qid = {.type = NINEP_QTDIR, .version = 0, .path = 6},
};
#endif

#if IS_ENABLED(CONFIG_NINEP_DFU)
static struct ninep_fs_node firmware_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 7},
};
#endif

#if IS_ENABLED(CONFIG_MEMFAULT)
static struct ninep_fs_node mflt_node = {
	.qid = {.type = NINEP_QTFILE, .version = 0, .path = 8},
};
#endif

/* Filesystem operations */
static struct ninep_fs_node *fs_get_root(void *ctx)
{
	/* Force initialization to ensure qid is valid */
	kbd_root_node.qid.type = NINEP_QTDIR;
	kbd_root_node.qid.version = 0;
	kbd_root_node.qid.path = 1;

	return &kbd_root_node;
}

static struct ninep_fs_node *fs_walk(struct ninep_fs_node *parent,
                                      const char *name, uint16_t name_len,
                                      void *ctx)
{
	/* Walk from root */
	if (parent == &kbd_root_node) {
		if (name_len == 4 && strncmp(name, "kbin", 4) == 0) {
			printk("[9P] WALK -> kbin\n");
			return &kbin_node;
		}
		if (name_len == 4 && strncmp(name, "leds", 4) == 0) {
			printk("[9P] WALK -> leds\n");
			return &leds_node;
		}
		if (name_len == 7 && strncmp(name, "battery", 7) == 0) {
			printk("[9P] WALK -> battery\n");
			return &battery_node;
		}
		if (name_len == 3 && strncmp(name, "ctl", 3) == 0) {
			printk("[9P] WALK -> ctl\n");
			return &ctl_node;
		}
		if (name_len == 7 && strncmp(name, "version", 7) == 0) {
			printk("[9P] WALK -> version\n");
			return &version_node;
		}
		if (name_len == 8 && strncmp(name, "settings", 8) == 0) {
			printk("[9P] WALK -> settings\n");
			return &settings_node;
		}
#if IS_ENABLED(CONFIG_NINEP_DFU) || IS_ENABLED(CONFIG_MEMFAULT)
		if (name_len == 3 && strncmp(name, "dev", 3) == 0) {
			printk("[9P] WALK -> dev\n");
			return &dev_node;
		}
#endif
		printk("[9P] WALK fail - unknown: %.*s\n", name_len, name);
		return NULL;
	}

	/* Walk from /settings */
	if (parent == &settings_node) {
		if (name_len == 8 && strncmp(name, "debounce", 8) == 0) {
			printk("[9P] WALK -> settings/debounce\n");
			return &debounce_node;
		}
		if (name_len == 4 && strncmp(name, "idle", 4) == 0) {
			printk("[9P] WALK -> settings/idle\n");
			return &idle_node;
		}
		printk("[9P] WALK fail - unknown in settings: %.*s\n", name_len, name);
		return NULL;
	}

#if IS_ENABLED(CONFIG_NINEP_DFU) || IS_ENABLED(CONFIG_MEMFAULT)
	/* Walk from /dev */
	if (parent == &dev_node) {
#if IS_ENABLED(CONFIG_NINEP_DFU)
		if (name_len == 8 && strncmp(name, "firmware", 8) == 0) {
			printk("[9P] WALK -> dev/firmware\n");
			return &firmware_node;
		}
#endif
#if IS_ENABLED(CONFIG_MEMFAULT)
		if (name_len == 4 && strncmp(name, "mflt", 4) == 0) {
			printk("[9P] WALK -> dev/mflt\n");
			return &mflt_node;
		}
#endif
		printk("[9P] WALK fail - unknown in dev: %.*s\n", name_len, name);
		return NULL;
	}
#endif

	printk("[9P] WALK fail - invalid parent\n");
	return NULL;
}

static int fs_open(struct ninep_fs_node *node, uint8_t mode, void *ctx)
{
	if (node == &kbin_node) {
		printk("[9P] OPEN kbin - setting connected=1\n");
		atomic_set(&kbin_client_connected, 1);
	} else if (node == &leds_node) {
		printk("[9P] OPEN leds\n");
	}
	/* Allow any mode for now */
	return 0;
}

static int fs_read(struct ninep_fs_node *node, uint64_t offset,
                   uint8_t *buf, uint32_t count, const char *uname, void *ctx)
{
	ARG_UNUSED(uname);
	if (node == &kbin_node) {
		/*
		 * Blocking read on kbin - canonical Plan 9/Unix behavior.
		 * Block until scan codes are available or timeout expires.
		 * This dramatically reduces BLE radio duty cycle vs polling.
		 *
		 * NOTE: This works because L2CAP transport uses a DEDICATED
		 * workqueue (ninep_workqueue) instead of the system workqueue.
		 * Blocking here won't affect ZMK event processing.
		 *
		 * TODO: For true 9P multiplexing, we need the server to handle
		 * blocking reads on a separate thread so other requests (battery,
		 * DFU, etc.) can be processed concurrently.
		 */
		size_t read_count = 0;

		while (read_count == 0) {
			/* Check if client disconnected */
			if (!atomic_get(&kbin_client_connected)) {
				LOG_WRN("kbin: client disconnected, aborting read");
				return -EIO;
			}

			/* Check if data is available */
			k_mutex_lock(&scancode_mutex, K_FOREVER);

			size_t available = 0;
			if (scancode_head >= scancode_tail) {
				available = scancode_head - scancode_tail;
			} else {
				available = SCANCODE_BUF_SIZE - scancode_tail + scancode_head;
			}

			if (available > 0) {
				/* Data available - read it */
				size_t to_read = MIN(count, available);

				while (read_count < to_read && scancode_tail != scancode_head) {
					buf[read_count++] = scancode_buf[scancode_tail];
					scancode_tail = (scancode_tail + 1) % SCANCODE_BUF_SIZE;
				}

				k_mutex_unlock(&scancode_mutex);
				printk("[9P] kbin -%zu bytes\n", read_count);
				return read_count;
			}

			k_mutex_unlock(&scancode_mutex);

			/*
			 * No data available - block until signaled or timeout.
			 * Timeout ensures connection stays alive and client can
			 * detect server health. Returns empty read on timeout.
			 */
			int ret = k_sem_take(&kbin_data_sem, K_MSEC(KBIN_READ_TIMEOUT_MS));
			if (ret == -EAGAIN) {
				/* Timeout - return empty read for keepalive */
				return 0;
			}
			/* Semaphore signaled - loop back to check for data */
		}

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

	if (node == &battery_node) {
		/* Read battery as "percentage millivolts\n" (e.g., "85 4023\n") */
		printk("[9P] battery read offset=%llu\n", offset);
		if (offset > 0) {
			printk("[9P] battery EOF\n");
			return 0;  /* Already read, no more data */
		}
		printk("[9P] battery fetching...\n");
		uint8_t pct = zmk_battery_state_of_charge();
		int mv = 0;

		/* Get raw millivolts from sensor */
		if (battery_dev && device_is_ready(battery_dev)) {
			struct sensor_value voltage;
			printk("[9P] battery sensor fetch...\n");
			if (sensor_sample_fetch_chan(battery_dev, SENSOR_CHAN_GAUGE_VOLTAGE) == 0 &&
			    sensor_channel_get(battery_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage) == 0) {
				mv = voltage.val1 * 1000 + voltage.val2 / 1000;
			}
			printk("[9P] battery sensor done\n");
		}

		int len = snprintf((char *)buf, count, "%u %d\n", pct, mv);
		printk("[9P] battery: %u%% %dmV\n", pct, mv);
		if (len < 0) {
			return -EIO;
		}
		return MIN(len, (int)count);
	}

	if (node == &ctl_node) {
		/* Read available control commands */
		if (offset > 0) {
			return 0;  /* Already read */
		}
		const char *help = "reboot - restart device\n";
		size_t len = strlen(help);
		size_t to_copy = MIN(len, (size_t)count);
		memcpy(buf, help, to_copy);
		return to_copy;
	}

	if (node == &version_node) {
		/* Read firmware version info */
		if (offset > 0) {
			return 0;  /* Already read */
		}

		char info[384];
		int len = 0;

		/* Product identifier */
		len += snprintf(info + len, sizeof(info) - len,
		                "product %s\n", FRST_PRODUCT);

		/* Firmware version */
		len += snprintf(info + len, sizeof(info) - len,
		                "version %s\n", FRST_FW_VERSION);

		/* Git build info from Zephyr build system */
#ifdef APP_BUILD_VERSION
		len += snprintf(info + len, sizeof(info) - len,
		                "build %s\n", STRINGIFY(APP_BUILD_VERSION));
#endif

		/* Zephyr kernel version */
		len += snprintf(info + len, sizeof(info) - len,
		                "zephyr %s\n", KERNEL_VERSION_STRING);

		/* ZMK app version */
		len += snprintf(info + len, sizeof(info) - len,
		                "zmk %s\n", APP_VERSION_STRING);

		/* Device serial from chip ID */
		uint8_t hwid[8];
		ssize_t hwid_len = hwinfo_get_device_id(hwid, sizeof(hwid));
		if (hwid_len > 0) {
			len += snprintf(info + len, sizeof(info) - len,
			                "serial %02X%02X%02X%02X\n",
			                hwid[0], hwid[1],
			                hwid[hwid_len > 2 ? 2 : 0],
			                hwid[hwid_len > 3 ? 3 : 0]);
		}

		/* Uptime in seconds */
		len += snprintf(info + len, sizeof(info) - len,
		                "uptime %lld\n", k_uptime_get() / 1000);

		size_t to_copy = MIN((size_t)len, (size_t)count);
		memcpy(buf, info, to_copy);
		return to_copy;
	}

	if (node == &debounce_node) {
		/* Read debounce settings */
		if (offset > 0) {
			return 0;
		}

		char info[128];
		int len = 0;

		if (kscan_dev) {
			uint32_t press_ms, release_ms;
			int32_t scan_ms;
			zmk_kscan_matrix_get_debounce_press_ms(kscan_dev, &press_ms);
			zmk_kscan_matrix_get_debounce_release_ms(kscan_dev, &release_ms);
			zmk_kscan_matrix_get_debounce_scan_period_ms(kscan_dev, &scan_ms);

			len += snprintf(info + len, sizeof(info) - len,
			                "press_ms %u\n", press_ms);
			len += snprintf(info + len, sizeof(info) - len,
			                "release_ms %u\n", release_ms);
			len += snprintf(info + len, sizeof(info) - len,
			                "scan_period_ms %d\n", scan_ms);
		} else {
			len += snprintf(info + len, sizeof(info) - len,
			                "error no_kscan_device\n");
		}

		size_t to_copy = MIN((size_t)len, (size_t)count);
		memcpy(buf, info, to_copy);
		return to_copy;
	}

	if (node == &idle_node) {
		/* Read idle/sleep settings */
		if (offset > 0) {
			return 0;
		}

		char info[128];
		int len = 0;

		len += snprintf(info + len, sizeof(info) - len,
		                "idle_timeout_ms %u\n", zmk_activity_get_idle_timeout_ms());

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
		len += snprintf(info + len, sizeof(info) - len,
		                "sleep_timeout_ms %u\n", zmk_activity_get_sleep_timeout_ms());
		len += snprintf(info + len, sizeof(info) - len,
		                "sleep_enabled %s\n",
		                zmk_activity_get_sleep_enabled() ? "true" : "false");
#else
		len += snprintf(info + len, sizeof(info) - len,
		                "sleep_enabled false\n");
#endif

		size_t to_copy = MIN((size_t)len, (size_t)count);
		memcpy(buf, info, to_copy);
		return to_copy;
	}

#if IS_ENABLED(CONFIG_NINEP_DFU)
	if (node == &firmware_node) {
		/* Read firmware status */
		if (offset > 0) {
			return 0;  /* Already read */
		}

		char status[256];
		int len = 0;

		/* State */
		len += snprintf(status + len, sizeof(status) - len,
		                "state %s\n", dfu_state_names[dfu.state]);

		/* Bytes written (during upload) */
		if (dfu.state == DFU_RECEIVING) {
			len += snprintf(status + len, sizeof(status) - len,
			                "bytes %u\n", dfu.bytes_written);
		}

		/* Error code (on error) */
		if (dfu.state == DFU_ERROR) {
			len += snprintf(status + len, sizeof(status) - len,
			                "error %d\n", dfu.last_error);
		}

		/* Current image version (slot0) */
		struct mcuboot_img_header hdr;
		int ret = boot_read_bank_header(FIXED_PARTITION_ID(slot0_partition),
		                                &hdr, sizeof(hdr));
		if (ret == 0 && hdr.mcuboot_version == 1) {
			len += snprintf(status + len, sizeof(status) - len,
			                "current %d.%d.%d+%d\n",
			                hdr.h.v1.sem_ver.major,
			                hdr.h.v1.sem_ver.minor,
			                hdr.h.v1.sem_ver.revision,
			                hdr.h.v1.sem_ver.build_num);
		}

		/* Pending image version (slot1) */
		ret = boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition),
		                            &hdr, sizeof(hdr));
		if (ret == 0 && hdr.mcuboot_version == 1) {
			len += snprintf(status + len, sizeof(status) - len,
			                "pending %d.%d.%d+%d\n",
			                hdr.h.v1.sem_ver.major,
			                hdr.h.v1.sem_ver.minor,
			                hdr.h.v1.sem_ver.revision,
			                hdr.h.v1.sem_ver.build_num);
		}

		/* Confirmation status */
		len += snprintf(status + len, sizeof(status) - len,
		                "confirmed %s\n",
		                boot_is_img_confirmed() ? "yes" : "no");

		size_t to_copy = MIN((size_t)len, (size_t)count);
		memcpy(buf, status, to_copy);
		return to_copy;
	}
#endif

#if IS_ENABLED(CONFIG_MEMFAULT)
	if (node == &mflt_node) {
		/*
		 * Memfault chunk export - each read returns one chunk.
		 * ESP32 can relay these to Memfault cloud via HTTP POST.
		 * Returns 0 bytes when no more data is available.
		 */
		if (offset > 0) {
			/* Chunks are stateless - no offset support */
			return 0;
		}

		size_t chunk_len = count;
		bool has_data = memfault_packetizer_get_chunk(buf, &chunk_len);

		if (has_data && chunk_len > 0) {
			LOG_INF("mflt: exported chunk (%zu bytes)", chunk_len);
			return chunk_len;
		}

		/* No more data available */
		return 0;
	}
#endif

	return -EINVAL;
}

static int fs_write(struct ninep_fs_node *node, uint64_t offset,
                    const uint8_t *buf, uint32_t count, const char *uname, void *ctx)
{
	ARG_UNUSED(uname);
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

	if (node == &ctl_node) {
		/* Handle control commands */
		if (count >= 6 && strncmp((const char *)buf, "reboot", 6) == 0) {
			LOG_INF("Received reboot command via /ctl");
			printk("[9P] REBOOT command received\n");

			/* Schedule reboot after 500ms to allow response to be sent */
			k_sleep(K_MSEC(100));
			sys_reboot(SYS_REBOOT_COLD);

			/* Won't reach here */
			return count;
		}

		LOG_WRN("Unknown ctl command: %.*s", count, buf);
		return -EINVAL;
	}

	if (node == &debounce_node) {
		/* Parse and apply debounce settings */
		/* Format: "key value\n" e.g., "press_ms 5" */
		if (!kscan_dev) {
			return -ENODEV;
		}

		char cmd[64];
		size_t cmd_len = MIN(count, sizeof(cmd) - 1);
		memcpy(cmd, buf, cmd_len);
		cmd[cmd_len] = '\0';

		/* Strip trailing newline */
		if (cmd_len > 0 && cmd[cmd_len - 1] == '\n') {
			cmd[cmd_len - 1] = '\0';
		}

		char *space = strchr(cmd, ' ');
		if (!space) {
			return -EINVAL;
		}
		*space = '\0';
		const char *key = cmd;
		uint32_t value = strtoul(space + 1, NULL, 10);

		if (strcmp(key, "press_ms") == 0) {
			zmk_kscan_matrix_set_debounce_press_ms(kscan_dev, value);
			frst_settings_save_debounce_press_ms(value);
			LOG_INF("Set debounce press_ms = %u (saved)", value);
		} else if (strcmp(key, "release_ms") == 0) {
			zmk_kscan_matrix_set_debounce_release_ms(kscan_dev, value);
			frst_settings_save_debounce_release_ms(value);
			LOG_INF("Set debounce release_ms = %u (saved)", value);
		} else if (strcmp(key, "scan_period_ms") == 0) {
			zmk_kscan_matrix_set_debounce_scan_period_ms(kscan_dev, (int32_t)value);
			frst_settings_save_debounce_scan_period_ms((int32_t)value);
			LOG_INF("Set debounce scan_period_ms = %u (saved)", value);
		} else {
			LOG_WRN("Unknown debounce key: %s", key);
			return -EINVAL;
		}

		return count;
	}

	if (node == &idle_node) {
		/* Parse and apply idle settings */
		/* Format: "key value\n" e.g., "idle_timeout_ms 60000" */
		char cmd[64];
		size_t cmd_len = MIN(count, sizeof(cmd) - 1);
		memcpy(cmd, buf, cmd_len);
		cmd[cmd_len] = '\0';

		/* Strip trailing newline */
		if (cmd_len > 0 && cmd[cmd_len - 1] == '\n') {
			cmd[cmd_len - 1] = '\0';
		}

		char *space = strchr(cmd, ' ');
		if (!space) {
			return -EINVAL;
		}
		*space = '\0';
		const char *key = cmd;
		const char *val_str = space + 1;

		if (strcmp(key, "idle_timeout_ms") == 0) {
			uint32_t value = strtoul(val_str, NULL, 10);
			zmk_activity_set_idle_timeout_ms(value);
			frst_settings_save_idle_timeout_ms(value);
			LOG_INF("Set idle_timeout_ms = %u (saved)", value);
		}
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
		else if (strcmp(key, "sleep_timeout_ms") == 0) {
			uint32_t value = strtoul(val_str, NULL, 10);
			zmk_activity_set_sleep_timeout_ms(value);
			frst_settings_save_sleep_timeout_ms(value);
			LOG_INF("Set sleep_timeout_ms = %u (saved)", value);
		} else if (strcmp(key, "sleep_enabled") == 0) {
			bool enabled = (strcmp(val_str, "true") == 0 || strcmp(val_str, "1") == 0);
			zmk_activity_set_sleep_enabled(enabled);
			/* Save as timeout value: 0 = disabled, default = enabled */
			frst_settings_save_sleep_timeout_ms(
				enabled ? CONFIG_ZMK_IDLE_SLEEP_TIMEOUT : 0);
			LOG_INF("Set sleep_enabled = %s (saved)", enabled ? "true" : "false");
		}
#endif
		else {
			LOG_WRN("Unknown idle key: %s", key);
			return -EINVAL;
		}

		return count;
	}

#if IS_ENABLED(CONFIG_NINEP_DFU)
	if (node == &firmware_node) {
		int ret;

		/* First write starts the upload - erase slot and init context */
		if (dfu.state != DFU_RECEIVING) {
			if (dfu.state == DFU_RECEIVING) {
				LOG_WRN("DFU already in progress, resetting");
			}

			dfu.state = DFU_ERASING;
			LOG_INF("DFU: erasing secondary slot...");

			ret = boot_erase_img_bank(FIXED_PARTITION_ID(slot1_partition));
			if (ret < 0) {
				LOG_ERR("Failed to erase secondary slot: %d", ret);
				dfu.state = DFU_ERROR;
				dfu.last_error = ret;
				return ret;
			}
			LOG_INF("DFU: secondary slot erased");

			ret = flash_img_init(&dfu.flash_ctx);
			if (ret < 0) {
				LOG_ERR("Failed to init flash_img context: %d", ret);
				dfu.state = DFU_ERROR;
				dfu.last_error = ret;
				return ret;
			}

			dfu.bytes_written = 0;
			dfu.last_progress_log = 0;
			dfu.state = DFU_RECEIVING;
			LOG_INF("DFU: ready to receive firmware");
		}

		/* Write chunk to flash */
		ret = flash_img_buffered_write(&dfu.flash_ctx, buf, count, false);
		if (ret < 0) {
			LOG_ERR("Flash write failed: %d", ret);
			dfu.state = DFU_ERROR;
			dfu.last_error = ret;
			return ret;
		}

		dfu.bytes_written += count;

		/* Progress logging every 50KB */
		if ((dfu.bytes_written / DFU_PROGRESS_LOG_INTERVAL) >
		    (dfu.last_progress_log / DFU_PROGRESS_LOG_INTERVAL)) {
			LOG_INF("DFU: %u bytes received", dfu.bytes_written);
			dfu.last_progress_log = dfu.bytes_written;
		}

		return count;
	}
#endif

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
	} else if (node == &battery_node) {
		name = "battery";
		mode = 0444;  // Read-only file
		length = 10;  // "100 4200\n" max
	} else if (node == &ctl_node) {
		name = "ctl";
		mode = 0644;  // Read-write file
		length = 32;  // Help text length
	} else if (node == &version_node) {
		name = "version";
		mode = 0444;  // Read-only file
		length = 256; // Version info text
	} else if (node == &settings_node) {
		name = "settings";
		mode = 0x80000000 | 0755;  // Directory
		length = 0;
	} else if (node == &debounce_node) {
		name = "debounce";
		mode = 0644;  // Read-write file
		length = 64;  // Settings text
	} else if (node == &idle_node) {
		name = "idle";
		mode = 0644;  // Read-write file
		length = 128; // Settings text
#if IS_ENABLED(CONFIG_NINEP_DFU) || IS_ENABLED(CONFIG_MEMFAULT)
	} else if (node == &dev_node) {
		name = "dev";
		mode = 0x80000000 | 0755;  // Directory with rwxr-xr-x
		length = 0;
#endif
#if IS_ENABLED(CONFIG_NINEP_DFU)
	} else if (node == &firmware_node) {
		name = "firmware";
		mode = 0644;  // Read-write file
		length = 0;   // Variable size
#endif
#if IS_ENABLED(CONFIG_MEMFAULT)
	} else if (node == &mflt_node) {
		name = "mflt";
		mode = 0444;  // Read-only file
		length = 0;   // Stream - size unknown
#endif
	} else {
		return -EINVAL;
	}

	uid = "zmk";
	gid = "zmk";
	muid = "zmk";

	/* Write stat using ninep_write_stat helper */
	int ret = ninep_write_stat(buf, buf_size, &offset, &node->qid, mode,
	                            length, name, strlen(name), uid, gid, muid);
	if (ret < 0) {
		return ret;
	}

	/* ninep_write_stat returns offset, we need to return total bytes written */
	return offset;
}

#if IS_ENABLED(CONFIG_NINEP_DFU)
static int fs_clunk(struct ninep_fs_node *node, void *ctx)
{
	if (node == &firmware_node && dfu.state == DFU_RECEIVING) {
		int ret;

		dfu.state = DFU_FINALIZING;
		LOG_INF("DFU: flushing buffer (%u bytes total)...", dfu.bytes_written);

		/* Flush remaining buffered data */
		ret = flash_img_buffered_write(&dfu.flash_ctx, NULL, 0, true);
		if (ret < 0) {
			LOG_ERR("Failed to flush final data: %d", ret);
			dfu.state = DFU_ERROR;
			dfu.last_error = ret;
			return ret;
		}

		LOG_INF("DFU: validating image...");

		/* Validate image header */
		struct mcuboot_img_header hdr;
		ret = boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition),
		                            &hdr, sizeof(hdr));
		if (ret < 0) {
			LOG_ERR("Failed to read image header: %d", ret);
			dfu.state = DFU_ERROR;
			dfu.last_error = ret;
			return ret;
		}

		if (hdr.mcuboot_version != 1) {
			LOG_ERR("Invalid MCUboot image version: %d", hdr.mcuboot_version);
			dfu.state = DFU_ERROR;
			dfu.last_error = -EINVAL;
			return -EINVAL;
		}

		LOG_INF("DFU: image v%d.%d.%d+%d validated",
		        hdr.h.v1.sem_ver.major,
		        hdr.h.v1.sem_ver.minor,
		        hdr.h.v1.sem_ver.revision,
		        hdr.h.v1.sem_ver.build_num);

		/* Mark for test upgrade */
		ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
		if (ret < 0) {
			LOG_ERR("Failed to mark image for upgrade: %d", ret);
			dfu.state = DFU_ERROR;
			dfu.last_error = ret;
			return ret;
		}

		dfu.state = DFU_COMPLETE;
		LOG_INF("DFU: complete - reboot to apply");
	}
	return 0;
}
#endif

static const struct ninep_fs_ops kbd_fs_ops = {
	.get_root = fs_get_root,
	.walk = fs_walk,
	.open = fs_open,
	.read = fs_read,
	.write = fs_write,
	.stat = fs_stat,
	.create = NULL,
	.remove = NULL,
#if IS_ENABLED(CONFIG_NINEP_DFU)
	.clunk = fs_clunk,
#else
	.clunk = NULL,
#endif
};

/* 9P server and transport */
static struct ninep_server kbd_server;
static struct ninep_transport kbd_transport;
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* BLE advertising - dynamic name with MAC suffix for 1:1 terminal pairing */
#ifdef CONFIG_ZMK_KEYBOARD_NAME
#define BLE_BASE_NAME CONFIG_ZMK_KEYBOARD_NAME
#else
#define BLE_BASE_NAME "FRST-KB"
#endif

/* Buffer for dynamic device name: "FRST-M1KB-A3F2" (base + "-" + 4 hex chars + null) */
#define BLE_NAME_MAX_LEN 32
static char ble_device_name[BLE_NAME_MAX_LEN];
static uint8_t ble_device_name_len;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x01, 0x10),  // 0x1001 for ESP32 discovery
#if IS_ENABLED(CONFIG_NINEP_GATT_9PIS)
	/* 9PIS service UUID: 39500001-feed-4a91-ba88-a1e0f6e4c001 (little-endian) */
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		0x01, 0xc0, 0xe4, 0xf6, 0xe0, 0xa1, 0x88, 0xba,
		0x91, 0x4a, 0xed, 0xfe, 0x01, 0x00, 0x50, 0x39),
#endif
	/* Short name omitted - full name with MAC suffix in scan response */
};

/* Scan response data - built dynamically with MAC suffix */
static struct bt_data sd[1];

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err) {
		LOG_ERR("BLE connection failed (err %u)", err);
		return;
	}

	/* Note: kbin_client_connected is now set in fs_open() when client opens kbin,
	 * not here on BLE connect. This ensures proper state after reconnect.
	 */
	printk("[STATE] BLE connected - awaiting 9P attach (connected=%d, sem=%u)\n",
	       (int)atomic_get(&kbin_client_connected),
	       k_sem_count_get(&kbin_data_sem));

	/* Turn on status LED to indicate connection */
	status_led_set(true);

	if (bt_conn_get_info(conn, &info) == 0) {
		LOG_INF("BLE connected: role=%s, interval=%u, latency=%u, timeout=%u, sec_level=%d",
		        info.role == BT_CONN_ROLE_CENTRAL ? "central" : "peripheral",
		        info.le.interval, info.le.latency, info.le.timeout,
		        info.security.level);
	} else {
		LOG_INF("BLE connected");
	}

	LOG_INF("L2CAP server ready on PSM 0x%04x - waiting for channel connections",
	        CONFIG_NINEP_L2CAP_PSM);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("BLE disconnected (reason %u)", reason);

	/* Turn off status LED */
	status_led_set(false);

	/* Mark client as disconnected and wake any blocked reader */
	atomic_set(&kbin_client_connected, 0);
	printk("[STATE] Disconnected - waking blocked readers\n");
	k_sem_give(&kbin_data_sem);

	/* Reset semaphore to clean state for next connection */
	k_sem_reset(&kbin_data_sem);

	/* Clear any buffered scancodes from previous session */
	k_mutex_lock(&scancode_mutex, K_FOREVER);
	scancode_head = 0;
	scancode_tail = 0;
	k_mutex_unlock(&scancode_mutex);
	printk("[STATE] Reset: sem=0, buffer cleared, connected=0\n");
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
	if (err) {
		LOG_ERR("Security change failed: level=%d, err=%d", level, err);
	} else {
		LOG_INF("Security level changed to %d", level);
	}
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                             uint16_t latency, uint16_t timeout)
{
	LOG_INF("LE params updated: interval=%u, latency=%u, timeout=%u",
	        interval, latency, timeout);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_updated = le_param_updated,
};

/* Initialize 9P keyboard server */
int kbd_9p_server_init(void)
{
	int ret;

	/* Initialize status LED */
	if (status_led.port && device_is_ready(status_led.port)) {
		ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
		if (ret == 0) {
			status_led_ready = true;
			LOG_INF("Status LED initialized (P0.15)");
		} else {
			LOG_WRN("Failed to configure status LED: %d", ret);
		}
	} else {
		LOG_WRN("Status LED not available");
	}

	/* Initialize BLE */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return ret;
	}
	LOG_INF("Bluetooth initialized");

	/* Build device name with MAC suffix for unique identification */
	{
		bt_addr_le_t addrs[1];
		size_t count = 1;

		bt_id_get(addrs, &count);
		if (count > 0) {
			/* Format: "FRST-M1KB-A3F2" (base name + last 2 bytes of MAC) */
			ble_device_name_len = snprintf(ble_device_name, BLE_NAME_MAX_LEN,
				"%s-%02X%02X",
				BLE_BASE_NAME,
				addrs[0].a.val[1],
				addrs[0].a.val[0]);
			LOG_INF("Device name: %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
				ble_device_name,
				addrs[0].a.val[5], addrs[0].a.val[4], addrs[0].a.val[3],
				addrs[0].a.val[2], addrs[0].a.val[1], addrs[0].a.val[0]);
		} else {
			/* Fallback if no address available */
			ble_device_name_len = snprintf(ble_device_name, BLE_NAME_MAX_LEN,
				"%s", BLE_BASE_NAME);
			LOG_WRN("No BLE address available, using base name: %s", ble_device_name);
		}

		/* Set up scan response with dynamic name */
		sd[0].type = BT_DATA_NAME_COMPLETE;
		sd[0].data_len = ble_device_name_len;
		sd[0].data = ble_device_name;
	}

#if IS_ENABLED(CONFIG_NINEP_GATT_9PIS)
	/* Initialize 9P Information Service (for iOS discovery) */
	static const struct ninep_9pis_config ninepisconfig = {
		.service_description = "9P Keyboard",
		.service_features = "keyboard,leds,dfu,battery,mflt",
		.transport_info = "l2cap:psm=0x0081",
		.app_store_link = "",
		.protocol_version = "9P2000;9p4z;1.0.0",
	};
	ret = ninep_9pis_init(&ninepisconfig);
	if (ret) {
		LOG_WRN("9PIS init failed (err %d) - iOS discovery may not work", ret);
		/* Continue anyway - 9PIS is optional */
	} else {
		LOG_INF("9PIS GATT service registered");
	}
#endif

	/* Initialize L2CAP transport */
	struct ninep_transport_l2cap_config l2cap_config = {
		.psm = CONFIG_NINEP_L2CAP_PSM,
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

	/* Start BLE advertising (name in scan response to fit both UUIDs in adv packet) */
	ret = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret) {
		LOG_ERR("Advertising failed to start (err %d)", ret);
		return ret;
	}
	LOG_INF("BLE advertising started");

	return 0;
}

/* Shell command to check 9P keyboard status */
static int cmd_kbd9p_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "=== 9P Keyboard Server Status ===");

	/* Show 9P server info */
	shell_print(sh, "9P Server: %s", kbd_server.transport ? "Running" : "Not initialized");
	shell_print(sh, "L2CAP PSM: 0x%04X", CONFIG_NINEP_L2CAP_PSM);
	shell_print(sh, "Max message size: %d", CONFIG_NINEP_MAX_MESSAGE_SIZE);
	shell_print(sh, "Advertising UUID: 0x1001");

	/* Check if BLE is enabled */
	if (bt_is_ready()) {
		shell_print(sh, "BLE: Enabled");
	} else {
		shell_print(sh, "BLE: NOT enabled - initialization may have failed");
		return 0;
	}

	/* Show dynamic channel config */
	shell_print(sh, "\n--- L2CAP Configuration ---");
	shell_print(sh, "Dynamic channels: %s",
	            IS_ENABLED(CONFIG_BT_L2CAP_DYNAMIC_CHANNEL) ? "ENABLED" : "DISABLED");
	shell_print(sh, "L2CAP TX MTU: %d", CONFIG_BT_L2CAP_TX_MTU);
	shell_print(sh, "Server security level: BT_SECURITY_L1 (no encryption)");

	/* Show scan code buffer status */
	k_mutex_lock(&scancode_mutex, K_FOREVER);
	size_t buffered = 0;
	if (scancode_head >= scancode_tail) {
		buffered = scancode_head - scancode_tail;
	} else {
		buffered = SCANCODE_BUF_SIZE - scancode_tail + scancode_head;
	}
	k_mutex_unlock(&scancode_mutex);

	shell_print(sh, "\n--- Runtime State ---");
	shell_print(sh, "Blocking reads: ENABLED (timeout %d sec)", KBIN_READ_TIMEOUT_MS / 1000);
	shell_print(sh, "Client connected: %s", atomic_get(&kbin_client_connected) ? "YES" : "NO");
	shell_print(sh, "Scan code buffer: %zu / %d", buffered, SCANCODE_BUF_SIZE);

	/* Show LED state */
	k_mutex_lock(&led_mutex, K_FOREVER);
	shell_print(sh, "LED state: 0x%02X (NumLock=%d CapsLock=%d ScrollLock=%d)",
	            led_state,
	            !!(led_state & 0x01),
	            !!(led_state & 0x02),
	            !!(led_state & 0x04));
	k_mutex_unlock(&led_mutex);

	/* Show battery level */
	int bat_mv = 0;
	if (battery_dev && device_is_ready(battery_dev)) {
		struct sensor_value voltage;
		if (sensor_sample_fetch_chan(battery_dev, SENSOR_CHAN_GAUGE_VOLTAGE) == 0 &&
		    sensor_channel_get(battery_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage) == 0) {
			bat_mv = voltage.val1 * 1000 + voltage.val2 / 1000;
		}
	}
	shell_print(sh, "Battery: %u%% (%d mV)", zmk_battery_state_of_charge(), bat_mv);

	shell_print(sh, "\nUse 'bt info' for BLE connection status");
	shell_print(sh, "Use 'l2cap register 0x81' in shell to test L2CAP server (will fail if already registered)");

	return 0;
}

/* Shell command to manually restart advertising */
static int cmd_kbd9p_advertise(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	/* Stop existing advertising if any */
	ret = bt_le_adv_stop();
	if (ret && ret != -EALREADY) {
		shell_error(sh, "Failed to stop advertising: %d", ret);
	}

	/* Restart advertising */
	ret = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret) {
		shell_error(sh, "Failed to start advertising: %d", ret);
		return ret;
	}

	shell_print(sh, "BLE advertising started");
	shell_print(sh, "  UUID: 0x1001 (advertising)");
	shell_print(sh, "  PSM: 0x%04X (L2CAP)", CONFIG_NINEP_L2CAP_PSM);

	return 0;
}

/* Shell command to reset device (MCUboot will run briefly then boot app) */
static int cmd_kbd9p_reset(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Resetting device in 1 second...");
	k_sleep(K_MSEC(1000));

	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

/* Shell command to test logging - verifies logs appear on console */
static int cmd_kbd9p_testlog(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Testing log output...");

	/* Test different log levels */
	LOG_ERR("TEST LOG: This is an ERROR level log from kbd_9p");
	LOG_WRN("TEST LOG: This is a WARNING level log from kbd_9p");
	LOG_INF("TEST LOG: This is an INFO level log from kbd_9p");
	LOG_DBG("TEST LOG: This is a DEBUG level log from kbd_9p");

	/* Also test printk */
	printk("TEST PRINTK: Direct printk output\n");

	shell_print(sh, "Log test complete. You should see 4 log lines above.");
	shell_print(sh, "(ERR, WRN, INF, DBG) plus a printk line.");
	return 0;
}

/* Shell command to inject a test scancode - verifies buffer and semaphore path */
static int cmd_kbd9p_testkey(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Injecting test scancode 0x1E ('A' press)...");
	add_scancode(0x1E);  /* 'A' key press */
	shell_print(sh, "Done. If blocking read is working, client should receive it.");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kbd9p,
	SHELL_CMD(status, NULL, "Show 9P keyboard server status", cmd_kbd9p_status),
	SHELL_CMD(advertise, NULL, "Start BLE advertising", cmd_kbd9p_advertise),
	SHELL_CMD(reset, NULL, "Reset device", cmd_kbd9p_reset),
	SHELL_CMD(testlog, NULL, "Test log output (verify logging works)", cmd_kbd9p_testlog),
	SHELL_CMD(testkey, NULL, "Inject test scancode to verify 9P path", cmd_kbd9p_testkey),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(kbd9p, &sub_kbd9p, "9P keyboard server commands", NULL);

/* Initialize at application level */
SYS_INIT(kbd_9p_server_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
