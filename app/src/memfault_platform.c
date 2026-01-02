/*
 * Copyright (c) 2025 FRST Keyboard Contributors
 * SPDX-License-Identifier: MIT
 */

#include <memfault/components.h>
#include <zephyr/drivers/hwinfo.h>
#include <stdio.h>

/* Version from build system - set by west sign --version */
#ifndef FRST_FW_VERSION
#define FRST_FW_VERSION "0.1.0-beta.1"
#endif

#define FRST_PRODUCT "FRST-M2KB"
#define FRST_HW_VERSION "nice_nano_v2"

/* Device serial derived from chip ID */
static char device_serial[32];
static bool serial_initialized = false;

static void init_device_serial(void)
{
	if (serial_initialized) {
		return;
	}

	uint8_t hwid[8];
	ssize_t len = hwinfo_get_device_id(hwid, sizeof(hwid));

	if (len > 0) {
		/* Format: FRST-M2KB-XXXX (last 4 hex chars of chip ID) */
		snprintf(device_serial, sizeof(device_serial),
			 "%s-%02X%02X", FRST_PRODUCT,
			 hwid[len > 1 ? len - 2 : 0],
			 hwid[len > 0 ? len - 1 : 0]);
	} else {
		snprintf(device_serial, sizeof(device_serial),
			 "%s-0000", FRST_PRODUCT);
	}
	serial_initialized = true;
}

void memfault_platform_get_device_info(sMemfaultDeviceInfo *info)
{
	init_device_serial();

	*info = (sMemfaultDeviceInfo) {
		.device_serial = device_serial,
		.software_type = "frst-m2kb-9p",
		.software_version = FRST_FW_VERSION,
		.hardware_version = FRST_HW_VERSION,
	};
}

/* Stub for custom root cert storage (we disabled HTTP) */
const char *memfault_http_client_get_root_cert(size_t *cert_len)
{
	*cert_len = 0;
	return NULL;
}
