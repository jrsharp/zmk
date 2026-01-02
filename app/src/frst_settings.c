/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 *
 * FRST Runtime Settings Persistence
 * Uses NVS to persist debounce and idle settings across reboots.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>
#include <zmk/kscan_settings.h>

LOG_MODULE_REGISTER(frst_settings, CONFIG_ZMK_LOG_LEVEL);

/* NVS partition - use storage partition */
#define NVS_PARTITION		storage_partition
#define NVS_PARTITION_DEVICE	FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET	FIXED_PARTITION_OFFSET(NVS_PARTITION)

/* NVS IDs for settings */
#define SETTINGS_ID_DEBOUNCE_PRESS	1
#define SETTINGS_ID_DEBOUNCE_RELEASE	2
#define SETTINGS_ID_DEBOUNCE_SCAN	3
#define SETTINGS_ID_IDLE_TIMEOUT	4
#define SETTINGS_ID_SLEEP_TIMEOUT	5

static struct nvs_fs frst_nvs;
static bool nvs_initialized = false;

/* Kscan device for debounce settings */
#if DT_HAS_CHOSEN(zmk_kscan)
static const struct device *const kscan_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_kscan));
#else
static const struct device *const kscan_dev = NULL;
#endif

static int frst_settings_init_nvs(void)
{
	int rc;
	struct flash_pages_info info;

	frst_nvs.flash_device = NVS_PARTITION_DEVICE;
	if (!device_is_ready(frst_nvs.flash_device)) {
		LOG_ERR("Flash device not ready");
		return -ENODEV;
	}

	frst_nvs.offset = NVS_PARTITION_OFFSET;
	rc = flash_get_page_info_by_offs(frst_nvs.flash_device, frst_nvs.offset, &info);
	if (rc) {
		LOG_ERR("Unable to get page info: %d", rc);
		return rc;
	}

	frst_nvs.sector_size = info.size;
	frst_nvs.sector_count = 3U;

	rc = nvs_mount(&frst_nvs);
	if (rc) {
		LOG_ERR("Flash Init failed: %d", rc);
		return rc;
	}

	nvs_initialized = true;
	LOG_INF("FRST settings NVS initialized");
	return 0;
}

static int frst_settings_load(void)
{
	int rc;
	uint32_t value;

	if (!nvs_initialized) {
		return -ENODEV;
	}

	/* Load debounce settings */
	if (kscan_dev) {
		rc = nvs_read(&frst_nvs, SETTINGS_ID_DEBOUNCE_PRESS, &value, sizeof(value));
		if (rc > 0) {
			zmk_kscan_matrix_set_debounce_press_ms(kscan_dev, value);
			LOG_INF("Loaded debounce press_ms = %u", value);
		}

		rc = nvs_read(&frst_nvs, SETTINGS_ID_DEBOUNCE_RELEASE, &value, sizeof(value));
		if (rc > 0) {
			zmk_kscan_matrix_set_debounce_release_ms(kscan_dev, value);
			LOG_INF("Loaded debounce release_ms = %u", value);
		}

		rc = nvs_read(&frst_nvs, SETTINGS_ID_DEBOUNCE_SCAN, &value, sizeof(value));
		if (rc > 0) {
			zmk_kscan_matrix_set_debounce_scan_period_ms(kscan_dev, (int32_t)value);
			LOG_INF("Loaded debounce scan_period_ms = %u", value);
		}
	}

	/* Load idle settings */
	rc = nvs_read(&frst_nvs, SETTINGS_ID_IDLE_TIMEOUT, &value, sizeof(value));
	if (rc > 0) {
		zmk_activity_set_idle_timeout_ms(value);
		LOG_INF("Loaded idle_timeout_ms = %u", value);
	}

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
	rc = nvs_read(&frst_nvs, SETTINGS_ID_SLEEP_TIMEOUT, &value, sizeof(value));
	if (rc > 0) {
		zmk_activity_set_sleep_timeout_ms(value);
		LOG_INF("Loaded sleep_timeout_ms = %u", value);
	}
#endif

	return 0;
}

/* Public API to save individual settings */
int frst_settings_save_debounce_press_ms(uint32_t value)
{
	if (!nvs_initialized) {
		return -ENODEV;
	}
	return nvs_write(&frst_nvs, SETTINGS_ID_DEBOUNCE_PRESS, &value, sizeof(value));
}

int frst_settings_save_debounce_release_ms(uint32_t value)
{
	if (!nvs_initialized) {
		return -ENODEV;
	}
	return nvs_write(&frst_nvs, SETTINGS_ID_DEBOUNCE_RELEASE, &value, sizeof(value));
}

int frst_settings_save_debounce_scan_period_ms(int32_t value)
{
	if (!nvs_initialized) {
		return -ENODEV;
	}
	uint32_t uval = (uint32_t)value;
	return nvs_write(&frst_nvs, SETTINGS_ID_DEBOUNCE_SCAN, &uval, sizeof(uval));
}

int frst_settings_save_idle_timeout_ms(uint32_t value)
{
	if (!nvs_initialized) {
		return -ENODEV;
	}
	return nvs_write(&frst_nvs, SETTINGS_ID_IDLE_TIMEOUT, &value, sizeof(value));
}

int frst_settings_save_sleep_timeout_ms(uint32_t value)
{
	if (!nvs_initialized) {
		return -ENODEV;
	}
	return nvs_write(&frst_nvs, SETTINGS_ID_SLEEP_TIMEOUT, &value, sizeof(value));
}

static int frst_settings_init(void)
{
	int rc;

	rc = frst_settings_init_nvs();
	if (rc) {
		LOG_WRN("Failed to initialize NVS, settings won't persist: %d", rc);
		return 0;  /* Don't fail boot */
	}

	rc = frst_settings_load();
	if (rc) {
		LOG_WRN("Failed to load settings: %d", rc);
	}

	return 0;
}

/* Initialize after kscan driver (priority 90) */
SYS_INIT(frst_settings_init, APPLICATION, 91);
