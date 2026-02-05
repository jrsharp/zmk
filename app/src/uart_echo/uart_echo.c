/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 *
 * UART Echo Test Module
 *
 * Simple echo server for verifying UART communication between
 * RP2040 keyboard and ESP32-S3 terminal before implementing 9P.
 *
 * All received bytes are echoed back immediately.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uart_echo, CONFIG_LOG_DEFAULT_LEVEL);

/* Ring buffer for received data */
#define RX_RING_SIZE 256
RING_BUF_DECLARE(rx_ring, RX_RING_SIZE);

/* TX state */
static const struct device *uart_dev;
static volatile bool tx_busy;

/* Statistics */
static uint32_t rx_count;
static uint32_t tx_count;

/*
 * UART interrupt callback
 */
static void uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev)) {
        return;
    }

    /* Handle RX */
    while (uart_irq_rx_ready(dev)) {
        uint8_t c;
        int ret = uart_fifo_read(dev, &c, 1);
        if (ret == 1) {
            ring_buf_put(&rx_ring, &c, 1);
            rx_count++;
        }
    }

    /* Handle TX */
    if (uart_irq_tx_ready(dev)) {
        uint8_t c;
        if (ring_buf_get(&rx_ring, &c, 1) == 1) {
            uart_fifo_fill(dev, &c, 1);
            tx_count++;
        } else {
            /* No more data to send, disable TX interrupt */
            uart_irq_tx_disable(dev);
            tx_busy = false;
        }
    }
}

/*
 * Work handler to enable TX when data is available
 */
static void echo_work_handler(struct k_work *work);
K_WORK_DEFINE(echo_work, echo_work_handler);

static void echo_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!tx_busy && !ring_buf_is_empty(&rx_ring)) {
        tx_busy = true;
        uart_irq_tx_enable(uart_dev);
    }
}

/*
 * Timer to periodically check for data to echo
 */
static void echo_timer_handler(struct k_timer *timer);
K_TIMER_DEFINE(echo_timer, echo_timer_handler, NULL);

static void echo_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&echo_work);
}

/*
 * Initialize UART echo test
 */
static int uart_echo_init(void)
{
    uart_dev = device_get_binding(CONFIG_UART_ECHO_DEVICE);
    if (uart_dev == NULL) {
        /* Try DT method */
        uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
    }

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device %s not ready", CONFIG_UART_ECHO_DEVICE);
        return -ENODEV;
    }

    /* Configure UART interrupt */
    uart_irq_callback_set(uart_dev, uart_isr);
    uart_irq_rx_enable(uart_dev);

    /* Start periodic timer to check for echo data (10ms interval) */
    k_timer_start(&echo_timer, K_MSEC(10), K_MSEC(10));

    LOG_INF("UART echo test started on %s", uart_dev->name);
    LOG_INF("All received bytes will be echoed back");

    return 0;
}

SYS_INIT(uart_echo_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/*
 * Shell command to show echo statistics (if shell is enabled)
 */
#if IS_ENABLED(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_echo_stats(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "UART Echo Statistics:");
    shell_print(sh, "  RX bytes: %u", rx_count);
    shell_print(sh, "  TX bytes: %u", tx_count);
    shell_print(sh, "  Ring buffer: %u/%u bytes",
                ring_buf_size_get(&rx_ring), RX_RING_SIZE);

    return 0;
}

SHELL_CMD_REGISTER(echo_stats, NULL, "Show UART echo statistics", cmd_echo_stats);
#endif /* CONFIG_SHELL */
