# FRST Keyboard Firmware Build Guide

This guide covers building and flashing firmware for FRST keyboards.

## Supported Keyboards

| Model | Shield | Board | Description |
|-------|--------|-------|-------------|
| FRST-M2KB | `chocv` | nice_nano_v2 | Single-piece 36-key keyboard |
| FRST-M1KB | `corne_frst_left` / `corne_frst_right` | nice_nano_v2 | Split Corne (wired TRRS) |

## Prerequisites

```bash
# Ensure west and Zephyr SDK are installed
west --version
# Ensure you're in the ZMK directory
cd /path/to/zmk
```

---

## Building FRST-M2KB (ChocV)

Single-piece keyboard with 9P over BLE L2CAP.

```bash
# Build
west build -p -s app -b nice_nano_v2 -d app/build_chocv -- -DSHIELD=chocv

# Output: app/build_chocv/zephyr/zmk.uf2
```

### Flashing M2KB

1. Double-tap the reset button on nice!nano
2. Device appears as USB drive (NICENANO)
3. Drag `zmk.uf2` to the drive
4. Device auto-reboots with new firmware

---

## Building FRST-M1KB (Corne Split)

Split keyboard with wired TRRS between halves. Main half connects to ESP32 terminal via 9P/BLE.

### Main Half (Left)

```bash
# Build
west build -p -s app -b nice_nano_v2 -d app/build_corne_left -- -DSHIELD=corne_frst_left

# Output: app/build_corne_left/zephyr/zmk.uf2
```

### Secondary Half (Right)

```bash
# Build
west build -p -s app -b nice_nano_v2 -d app/build_corne_right -- -DSHIELD=corne_frst_right

# Output: app/build_corne_right/zephyr/zmk.uf2
```

### Flashing M1KB

Flash each half separately:

1. **Secondary (right) first** - Double-tap reset, drag `zmk.uf2`
2. **Main (left) second** - Double-tap reset, drag `zmk.uf2`
3. Connect TRRS cable between halves
4. Power on - main half will communicate with secondary via UART

---

## Device Naming & Pairing

Each keyboard advertises with a unique name based on its MAC address:

```
FRST-M2KB-A3F2    (M2KB with MAC ending in A3:F2)
FRST-M1KB-B7C1    (M1KB with MAC ending in B7:C1)
```

### Finding Your Keyboard's MAC

On first boot, the keyboard logs its full MAC address:

```
Device name: FRST-M1KB-A3F2 (MAC: DE:AD:BE:EF:A3:F2)
```

View this via USB serial console (115200 baud) or BLE scan.

### Terminal Pairing

The ESP32 terminal should be configured with the target keyboard's MAC:

1. Scan for devices matching `FRST-M*KB-*`
2. Note the full MAC address of your keyboard
3. Store MAC in terminal's NVS or compile-time config
4. Terminal will only connect to that specific keyboard

### BLE Bonding

After first connection, devices bond (exchange encryption keys). Subsequent connections are:
- Encrypted (keystrokes protected)
- Faster (skip pairing handshake)
- Automatic (devices remember each other)

---

## Build Sizes

| Target | Flash | RAM | Notes |
|--------|-------|-----|-------|
| chocv (M2KB) | ~326KB | ~166KB | Full 9P, DFU, Memfault |
| corne_frst_left (M1KB main) | ~300KB | ~96KB | 9P, USB console |
| corne_frst_right (M1KB secondary) | ~31KB | ~7KB | Minimal (key scan + UART) |

---

## Troubleshooting

### Keyboard not advertising

Check USB console for errors:
```bash
# Connect to USB serial (macOS)
screen /dev/tty.usbmodem* 115200

# Or on Linux
screen /dev/ttyACM0 115200
```

### Split halves not communicating

1. Verify TRRS cable is fully seated
2. Check that both halves have matching firmware versions
3. Secondary half has no BLE - only communicates via TRRS

### Terminal can't find keyboard

1. Verify keyboard is powered and advertising (blue LED behavior)
2. Check MAC address matches terminal config
3. Try clearing bonds on both devices and re-pairing

---

## Configuration Reference

### Key Configs (corne_frst_left.conf)

```kconfig
# 9P over BLE L2CAP
CONFIG_NINEP=y
CONFIG_NINEP_TRANSPORT_L2CAP=y
CONFIG_NINEP_L2CAP_PSM=0x0081

# Wired split
CONFIG_ZMK_SPLIT=y
CONFIG_ZMK_SPLIT_WIRED=y

# Stable MAC for pairing
CONFIG_BT_PRIVACY=n
CONFIG_BT_BONDABLE=y
```

### Customizing Keyboard Name

Edit `Kconfig.defconfig` in the shield directory:

```kconfig
config ZMK_KEYBOARD_NAME
    default "FRST-M1KB"  # Base name, MAC suffix added automatically
```
