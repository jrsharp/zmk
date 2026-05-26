/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 *
 * Layer indicator LED for RP2040-Zero onboard WS2812
 * Bit-bangs WS2812 protocol on GP16 to show active layer color.
 *
 * Green  = Layer 0 (base)
 * Yellow = Layer 1 (numbers)
 * Red    = Layer 2 (symbols)
 *
 * Power-save: LED turns off after configurable idle timeout.
 * Wakes on any keypress. Timeout persisted in NVS.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <hardware/gpio.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(layer_indicator, CONFIG_LOG_DEFAULT_LEVEL);

#define LED_PIN 16
#define LED_DEFAULT_TIMEOUT_S 30

/*
 * WS2812 bit-bang using direct RP2040 SIO register access.
 */
#include <hardware/regs/addressmap.h>
#define GPIO_OUT_SET    (*(volatile uint32_t *)(SIO_BASE + 0x014))
#define GPIO_OUT_CLR    (*(volatile uint32_t *)(SIO_BASE + 0x018))
#define PIN_MASK        (1u << LED_PIN)

static inline void delay_short(void)
{
	__asm volatile(
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop;"
	);
}

static inline void delay_long(void)
{
	__asm volatile(
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
	);
}

static void ws2812_send_byte(uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		if (byte & (1 << i)) {
			GPIO_OUT_SET = PIN_MASK;
			delay_long();
			GPIO_OUT_CLR = PIN_MASK;
			delay_short();
		} else {
			GPIO_OUT_SET = PIN_MASK;
			delay_short();
			GPIO_OUT_CLR = PIN_MASK;
			delay_long();
		}
	}
}

void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
	unsigned int key = irq_lock();
	ws2812_send_byte(g);
	ws2812_send_byte(r);
	ws2812_send_byte(b);
	irq_unlock(key);
	k_busy_wait(300);
}

/*
 * LED state
 */
static bool led_on = true;
static uint32_t led_timeout_s = LED_DEFAULT_TIMEOUT_S;
static int64_t last_activity;

static void set_layer_color(uint8_t layer)
{
	switch (layer) {
	case 0:
		ws2812_set_color(32, 0, 0);    /* Green */
		break;
	case 1:
		ws2812_set_color(24, 32, 0);   /* Yellow */
		break;
	case 2:
		ws2812_set_color(0, 32, 0);    /* Red */
		break;
	default:
		ws2812_set_color(0, 0, 32);    /* Blue */
		break;
	}
}

static void led_off(void)
{
	ws2812_set_color(0, 0, 0);
	led_on = false;
}

static void led_wake(void)
{
	last_activity = k_uptime_get();
	if (!led_on) {
		led_on = true;
		set_layer_color(zmk_keymap_highest_layer_active());
	}
}

/*
 * Idle timer — checks periodically if LED should turn off
 */
static void idle_work_handler(struct k_work *work);
static void idle_timer_handler(struct k_timer *timer);

K_WORK_DEFINE(idle_work, idle_work_handler);
K_TIMER_DEFINE(idle_timer, idle_timer_handler, NULL);

static void idle_timer_handler(struct k_timer *timer)
{
	k_work_submit(&idle_work);
}

static void idle_work_handler(struct k_work *work)
{
	if (led_timeout_s == 0 || !led_on) {
		return;
	}

	int64_t idle_ms = k_uptime_get() - last_activity;
	if (idle_ms >= (int64_t)led_timeout_s * 1000) {
		led_off();
	}
}

/*
 * ZMK event listeners
 */
static int layer_listener(const zmk_event_t *eh)
{
	const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
	if (ev == NULL) return ZMK_EV_EVENT_BUBBLE;

	led_wake();
	set_layer_color(zmk_keymap_highest_layer_active());
	return ZMK_EV_EVENT_BUBBLE;
}

static int keycode_listener(const zmk_event_t *eh)
{
	const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
	if (ev == NULL) return ZMK_EV_EVENT_BUBBLE;

	led_wake();
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_indicator, layer_listener);
ZMK_SUBSCRIPTION(layer_indicator, zmk_layer_state_changed);

ZMK_LISTENER(layer_indicator_key, keycode_listener);
ZMK_SUBSCRIPTION(layer_indicator_key, zmk_keycode_state_changed);

/*
 * Settings persistence (NVS)
 */
static int led_settings_load(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
	if (!strcmp(name, "timeout")) {
		if (len != sizeof(led_timeout_s)) return -EINVAL;
		read_cb(cb_arg, &led_timeout_s, sizeof(led_timeout_s));
		LOG_INF("LED timeout loaded: %u s", led_timeout_s);
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(led, "led", NULL, led_settings_load, NULL, NULL);

/*
 * Public API for 9P cfg node
 */
uint32_t layer_indicator_get_timeout(void)
{
	return led_timeout_s;
}

void layer_indicator_set_timeout(uint32_t seconds)
{
	led_timeout_s = seconds;
	settings_save_one("led/timeout", &led_timeout_s, sizeof(led_timeout_s));
	LOG_INF("LED timeout set: %u s", led_timeout_s);

	if (seconds == 0) {
		/* Always on — wake LED if it was off */
		led_wake();
	}
}

/*
 * Init
 */
static int layer_indicator_init(void)
{
	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, true);
	gpio_put(LED_PIN, 0);

	last_activity = k_uptime_get();
	set_layer_color(0);

	/* Start idle check timer (every 5 seconds) */
	k_timer_start(&idle_timer, K_SECONDS(5), K_SECONDS(5));

	LOG_INF("Layer indicator on GP%d (timeout %u s)", LED_PIN, led_timeout_s);
	return 0;
}

SYS_INIT(layer_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
