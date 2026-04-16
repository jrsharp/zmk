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
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <hardware/gpio.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(layer_indicator, CONFIG_LOG_DEFAULT_LEVEL);

#define LED_PIN 16

/*
 * WS2812 bit-bang using direct RP2040 SIO register access.
 *
 * At 125 MHz, 1 cycle = 8 ns.
 * SIO GPIO set/clr is a single-cycle register write.
 *
 * WS2812 timing requirements:
 *   T0H: 220-380 ns,  T0L: 580-1000 ns
 *   T1H: 580-1000 ns, T1L: 220-420 ns
 */

#include <hardware/regs/addressmap.h>
#define GPIO_OUT_SET    (*(volatile uint32_t *)(SIO_BASE + 0x014))
#define GPIO_OUT_CLR    (*(volatile uint32_t *)(SIO_BASE + 0x018))

#define PIN_MASK        (1u << LED_PIN)

/*
 * Tuned for 125 MHz Cortex-M0+, 1 nop = 1 cycle = 8 ns.
 * T0H ~350 ns = ~44 nops,  T1H ~700 ns = ~88 nops
 * Account for ~5 cycles overhead from SET/CLR register writes + branch.
 */
static inline void delay_short(void)
{
	/* T0H ~300 ns = ~35 nops (+ overhead) */
	__asm volatile(
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
		"nop; nop; nop; nop; nop;"
	);
}

static inline void delay_long(void)
{
	/* T1H ~900 ns = ~100 nops (+ overhead) — generous for clear 1-bit */
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

/* WS2812 expects GRB byte order */
void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
	/*
	 * RP2040-Zero onboard LED is GRB order.
	 * If colors appear swapped (red↔green), the LED variant
	 * may be RGB — try swapping r/g here.
	 */
	unsigned int key = irq_lock();
	ws2812_send_byte(g);
	ws2812_send_byte(r);
	ws2812_send_byte(b);
	irq_unlock(key);
	/* Reset pulse (>280 µs low for reliable latch) */
	k_busy_wait(300);
}

static void set_layer_color(uint8_t layer)
{
	/*
	 * GRB byte order. Empirically:
	 *   ws2812_set_color(32,0,0) sent as G=0,R=32,B=0 → showed red
	 *   ws2812_set_color(0,32,0) sent as G=32,R=0,B=0 → showed red too
	 * This means byte 1 (sent as G) is actually driving RED.
	 * Real order appears to be: byte1=R, byte2=G, byte3=B
	 * So send order should be: ws2812_send_byte(r), send(g), send(b)
	 * But we're sending g,r,b. So swap the ARGUMENTS instead:
	 *   For green:  ws2812_set_color(g=32, r=0, b=0) → sends G=0,R=32→ still wrong
	 *
	 * Just use empirical mapping:
	 *   Byte 1 (g param → sent first) controls RED
	 *   Byte 2 (r param → sent second) controls GREEN
	 *   Byte 3 (b param → sent third) controls BLUE
	 */
	switch (layer) {
	case 0:
		ws2812_set_color(32, 0, 0);    /* Green: r param=32 → byte2 → green */
		break;
	case 1:
		ws2812_set_color(24, 32, 0);   /* Yellow: green+red */
		break;
	case 2:
		ws2812_set_color(0, 32, 0);    /* Red: g param=32 → byte1 → red */
		break;
	default:
		ws2812_set_color(0, 0, 32);    /* Blue */
		break;
	}
}

static int layer_indicator_listener(const zmk_event_t *eh)
{
	const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);

	if (ev == NULL) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	set_layer_color(zmk_keymap_highest_layer_active());
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_indicator, layer_indicator_listener);
ZMK_SUBSCRIPTION(layer_indicator, zmk_layer_state_changed);

static int layer_indicator_init(void)
{
	/* Use Pico SDK to init GPIO - matches our direct SIO register access */
	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, true);
	gpio_put(LED_PIN, 0);

	set_layer_color(0);

	LOG_INF("Layer indicator initialized on GP%d", LED_PIN);
	return 0;
}

SYS_INIT(layer_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
