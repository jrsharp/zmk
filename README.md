# ZMK, forked for the FRST Computer

This is a fork of [ZMK](https://zmk.dev/) carrying the keyboard firmware for
the [FRST Computer](https://frstcomputer.com/), an e-ink cyberdeck built on
Plan 9 ideas. Upstream ZMK's README follows below, unchanged.

**The keyboard is not an HID device here. It is a 9P file server.**

The deck mounts the keyboard into its namespace and reads keystrokes out of a
file. Battery level, layer state, debounce tuning and firmware upload are
files in that same namespace, so configuring the keyboard is a write to `ctl`
and updating it is a write to `firmware`. There is no vendor tool and no
second protocol, because a peripheral that speaks 9P is already a peer on the
network rather than a device hanging off a bus. That is the whole idea the
deck is built on, and the keyboard is where you meet it first.

## What this fork adds

Two transports for the same server:

- **Wired, over UART** (`app/src/uart_9p_server/`) — the shipping path.
  `CONFIG_UART_9P_SERVER=y`, non-blocking, USB off entirely.
- **Wireless, over BLE L2CAP** (`app/src/kbd_9p_server.c`) — 9P on a
  connection-oriented channel at PSM `0x0081`, advertised via a 9PIS GATT
  service so a deck can find a keyboard without pairing UI.

Four shields:

| shield | board | notes |
|---|---|---|
| `frst36` | `rpi_pico` | the M1KB: 36 keys, wired 9P, MCUboot DFU |
| `corne_frst` | `nice_nano_v2` | 42-key split, wired split fixes |
| `chocv` | `nice_nano_v2` | BLE L2CAP 9P, Secure Connections pairing |
| `chocv_hogp_test` | `nice_nano_v2` | stock HOGP, for power comparison |

Plus the core changes those need: battery reporting in millivolts, activity
and deep-sleep tuning, wired split battery fetch, stable MAC-derived device
naming for 1:1 pairing, a layer indicator, and runtime settings persisted to
NVS through the 9P `ctl` file.

## Building

A normal ZMK west workspace, with one extra module —
[9p4z](https://github.com/jrsharp/9p4z), the 9P library, pinned in
`app/west.yml`:

```sh
west init -l app && west update
west build -s app -b rpi_pico -- -DSHIELD=frst36
```

That produces `build/zephyr/zmk.uf2`. Hold BOOTSEL, drag it onto the volume
that appears; no toolchain needed on the receiving end.

`BUILD.md` covers the signed-image path (MCUboot + ECDSA-P256) for the
nice!nano targets.

## Licence

MIT, as upstream ZMK is. Files added by this fork carry
`Copyright (c) 2025 Jon Sharp` and `SPDX-License-Identifier: MIT`.

---

# Zephyr™ Mechanical Keyboard (ZMK) Firmware

[![Discord](https://img.shields.io/discord/719497620560543766)](https://zmk.dev/community/discord/invite)
[![Build](https://github.com/zmkfirmware/zmk/workflows/Build/badge.svg)](https://github.com/zmkfirmware/zmk/actions)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-v2.0%20adopted-ff69b4.svg)](CODE_OF_CONDUCT.md)

[ZMK Firmware](https://zmk.dev/) is an open source ([MIT](LICENSE)) keyboard firmware built on the [Zephyr™ Project](https://www.zephyrproject.org/) Real Time Operating System (RTOS). ZMK's goal is to provide a modern, wireless, and powerful firmware free of licensing issues.

Check out the website to learn more: https://zmk.dev/.

You can also come join our [ZMK Discord Server](https://zmk.dev/community/discord/invite).

To review features, check out the [feature overview](https://zmk.dev/docs/). ZMK is under active development, and new features are listed with the [enhancement label](https://github.com/zmkfirmware/zmk/issues?q=is%3Aissue+is%3Aopen+label%3Aenhancement) in GitHub. Please feel free to add 👍 to the issue description of any requests to upvote the feature.
