/*
 * Copyright (c) 2025 chocv Keyboard Contributors
 * SPDX-License-Identifier: MIT
 */

#include <memfault/components.h>

void memfault_platform_get_device_info(sMemfaultDeviceInfo *info)
{
	*info = (sMemfaultDeviceInfo) {
		.device_serial = "CHOCV-9P-DEBUG",
		.software_type = "zmk-9p",
		.software_version = "0.1.0-dev",
		.hardware_version = "nice_nano_v2",
	};
}

/* Stub for custom root cert storage (we disabled HTTP) */
const char *memfault_http_client_get_root_cert(size_t *cert_len)
{
	*cert_len = 0;
	return NULL;
}
