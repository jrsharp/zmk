/*
 * Copyright (c) 2025 Jon Sharp
 * Co-authored by Claude (Anthropic)
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <stdint.h>

/**
 * @brief Runtime debounce settings API for kscan matrix driver
 *
 * These functions allow runtime modification of debounce parameters
 * without requiring a rebuild. Settings take effect immediately.
 */

/**
 * @brief Get the current press debounce time
 * @param dev Pointer to the kscan device
 * @param ms Pointer to store the debounce time in milliseconds
 * @return 0 on success, negative error code on failure
 */
int zmk_kscan_matrix_get_debounce_press_ms(const struct device *dev, uint32_t *ms);

/**
 * @brief Set the press debounce time
 * @param dev Pointer to the kscan device
 * @param ms Debounce time in milliseconds (max 16383)
 * @return 0 on success, negative error code on failure
 */
int zmk_kscan_matrix_set_debounce_press_ms(const struct device *dev, uint32_t ms);

/**
 * @brief Get the current release debounce time
 * @param dev Pointer to the kscan device
 * @param ms Pointer to store the debounce time in milliseconds
 * @return 0 on success, negative error code on failure
 */
int zmk_kscan_matrix_get_debounce_release_ms(const struct device *dev, uint32_t *ms);

/**
 * @brief Set the release debounce time
 * @param dev Pointer to the kscan device
 * @param ms Debounce time in milliseconds (max 16383)
 * @return 0 on success, negative error code on failure
 */
int zmk_kscan_matrix_set_debounce_release_ms(const struct device *dev, uint32_t ms);

/**
 * @brief Get the current debounce scan period
 * @param dev Pointer to the kscan device
 * @param ms Pointer to store the scan period in milliseconds
 * @return 0 on success, negative error code on failure
 */
int zmk_kscan_matrix_get_debounce_scan_period_ms(const struct device *dev, int32_t *ms);

/**
 * @brief Set the debounce scan period
 * @param dev Pointer to the kscan device
 * @param ms Scan period in milliseconds (must be >= 1)
 * @return 0 on success, negative error code on failure
 */
int zmk_kscan_matrix_set_debounce_scan_period_ms(const struct device *dev, int32_t ms);
