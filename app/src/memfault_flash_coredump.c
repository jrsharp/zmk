/*
 * Copyright (c) 2025 chocv Contributors
 * SPDX-License-Identifier: MIT
 *
 * Flash-backed coredump storage for Memfault on nRF52840
 * This uses direct flash writes to the storage partition
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <memfault/panics/platform/coredump.h>
#include <memfault/core/debug_log.h>
#include <string.h>

/* Blue LED on nice_nano_v2 - P0.15 */
#define LED_GPIO_NODE DT_NODELABEL(gpio0)
#define LED_PIN 15

static void blink_led(int times) {
	const struct device *gpio = DEVICE_DT_GET(LED_GPIO_NODE);
	if (!device_is_ready(gpio)) {
		return;
	}

	gpio_pin_configure(gpio, LED_PIN, GPIO_OUTPUT_ACTIVE);

	for (int i = 0; i < times; i++) {
		gpio_pin_set(gpio, LED_PIN, 1);
		k_busy_wait(100000); /* 100ms */
		gpio_pin_set(gpio, LED_PIN, 0);
		k_busy_wait(100000); /* 100ms */
	}
}

#define COREDUMP_MAX_SIZE 32768     /* 32KB max coredump size - full storage partition */

static const struct flash_area *s_flash_area;
static bool s_flash_initialized = false;

static bool prv_init_flash(void)
{
	if (s_flash_initialized) {
		return true;
	}

	int ret = flash_area_open(FIXED_PARTITION_ID(coredump_partition), &s_flash_area);
	if (ret != 0) {
		MEMFAULT_LOG_ERROR("Failed to open flash area: %d", ret);
		return false;
	}

	s_flash_initialized = true;
	MEMFAULT_LOG_INFO("Flash coredump storage initialized (size=%u bytes)",
	                   s_flash_area->fa_size);
	return true;
}

void memfault_platform_coredump_storage_get_info(sMfltCoredumpStorageInfo *info)
{
	/* Debug: show this function is being called */
	printk(">>> memfault get_info called\n");

	if (!prv_init_flash()) {
		printk(">>> memfault get_info: init failed!\n");
		*info = (sMfltCoredumpStorageInfo){ .size = 0 };
		return;
	}

	*info = (sMfltCoredumpStorageInfo){
		.size = COREDUMP_MAX_SIZE,
		.sector_size = 4096,  /* nRF52840 flash page size */
	};

	printk(">>> memfault get_info: size=%u sector=%u\n",
	       (unsigned)info->size, (unsigned)info->sector_size);
}

bool memfault_platform_coredump_storage_read(uint32_t offset, void *data, size_t read_len)
{
	if (!prv_init_flash()) {
		return false;
	}

	if ((offset + read_len) > COREDUMP_MAX_SIZE) {
		return false;
	}

	int ret = flash_area_read(s_flash_area, offset, data, read_len);
	if (ret != 0) {
		MEMFAULT_LOG_ERROR("Flash read failed: %d", ret);
		return false;
	}

	/* Debug: log first read to see if coredump header exists */
	if (offset == 0 && read_len >= 12) {
		uint32_t *header = (uint32_t *)data;
		printk(">>> memfault read header: magic=0x%08x version=0x%08x size=0x%08x\n",
		       header[0], header[1], header[2]);
	}

	return true;
}

bool memfault_platform_coredump_storage_erase(uint32_t offset, size_t erase_size)
{
	/* Blink LED to show erase is being called - 2 rapid blinks */
	blink_led(2);

	/* Use printk during crash - logging may not work */
	printk(">>> memfault erase: offset=%u size=%u\n", offset, erase_size);

	if (!prv_init_flash()) {
		printk(">>> memfault erase: init failed\n");
		/* 5 slow blinks = init failed */
		blink_led(5);
		return false;
	}

	int ret = flash_area_erase(s_flash_area, offset, erase_size);
	if (ret != 0) {
		printk(">>> memfault erase: failed ret=%d\n", ret);
		MEMFAULT_LOG_ERROR("Flash erase failed: %d", ret);
		/* 4 blinks = erase failed */
		blink_led(4);
		return false;
	}

	printk(">>> memfault erase: success\n");
	return true;
}

bool memfault_platform_coredump_storage_write(uint32_t offset, const void *data, size_t data_len)
{
	/* Blink LED to show write is being called - 3 rapid blinks */
	blink_led(3);

	/* Use printk during crash - logging may not work */
	printk(">>> memfault write: offset=%u len=%u\n", offset, data_len);

	if (!prv_init_flash()) {
		printk(">>> memfault write: init failed\n");
		/* 5 slow blinks = init failed */
		blink_led(5);
		return false;
	}

	if ((offset + data_len) > COREDUMP_MAX_SIZE) {
		printk(">>> memfault write: too large\n");
		MEMFAULT_LOG_ERROR("Coredump too large: %u + %u > %u",
		                   offset, data_len, COREDUMP_MAX_SIZE);
		return false;
	}

	int ret = flash_area_write(s_flash_area, offset, data, data_len);
	if (ret != 0) {
		printk(">>> memfault write: failed ret=%d\n", ret);
		MEMFAULT_LOG_ERROR("Flash write failed: %d", ret);
		return false;
	}

	printk(">>> memfault write: success\n");
	MEMFAULT_LOG_DEBUG("Wrote %u bytes to flash coredump storage at offset %u",
	                    data_len, offset);
	return true;
}

void memfault_platform_coredump_storage_clear(void)
{
	if (!prv_init_flash()) {
		return;
	}

	/* Erase first 4K to mark coredump as invalid */
	int ret = flash_area_erase(s_flash_area, 0, 4096);
	if (ret != 0) {
		MEMFAULT_LOG_ERROR("Flash erase failed: %d", ret);
	} else {
		MEMFAULT_LOG_INFO("Coredump cleared from flash");
	}
}
