/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Save debounce settings to NVS */
int frst_settings_save_debounce_press_ms(uint32_t value);
int frst_settings_save_debounce_release_ms(uint32_t value);
int frst_settings_save_debounce_scan_period_ms(int32_t value);

/* Save idle settings to NVS */
int frst_settings_save_idle_timeout_ms(uint32_t value);
int frst_settings_save_sleep_timeout_ms(uint32_t value);
