
/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/zephyr.h>
#include <zmk/event_manager.h>
#include <zmk/mouse.h>
#include <dt-bindings/zmk/mouse.h>

struct zmk_mouse_scroll_state_changed {
    struct vector2d max_speed;
    struct mouse_config config;
    bool state;
    int64_t timestamp;
};

ZMK_EVENT_DECLARE(zmk_mouse_scroll_state_changed);

static inline struct zmk_mouse_scroll_state_changed_event *
zmk_mouse_scroll_state_changed_from_encoded(uint32_t encoded, struct mouse_config config,
                                            bool pressed, int64_t timestamp) {
    struct vector2d max_speed = (struct vector2d){
        .x = SCROLL_HOR_DECODE(encoded),
        .y = SCROLL_VERT_DECODE(encoded),
    };

    return new_zmk_mouse_scroll_state_changed((struct zmk_mouse_scroll_state_changed){
        .max_speed = max_speed, .config = config, .state = pressed, .timestamp = timestamp});
}
