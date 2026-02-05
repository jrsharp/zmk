# Copyright (c) 2025 Jon Sharp
# Co-authored by Claude (Anthropic)
# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_UART_ECHO_TEST uart_echo.c)
