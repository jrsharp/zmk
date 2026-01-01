# ChocV Keyboard Firmware Build Guide

This documents the complete build process for the ChocV 9P keyboard firmware,
including MCUboot bootloader and signed application.

## Overview

- **Target**: nice!nano v2 (nRF52840)
- **Bootloader**: MCUboot with ECDSA-P256 signature verification
- **Firmware**: ZMK-based 9P keyboard server over BLE L2CAP
- **Features**: Blocking key reads, concurrent 9P ops, DFU via /dev/firmware

## Prerequisites

```bash
# Activate ZMK virtualenv
cd /mnt2/src/zmk && source .venv/bin/activate

# Verify tools
west --version
nrfjprog --version
```

## Directory Structure

```
/mnt2/src/zmk/
├── app/                          # ZMK application
│   ├── boards/shields/chocv/     # ChocV shield config
│   ├── src/kbd_9p_server.c       # 9P keyboard server
│   └── build/                    # Build output
├── bootloader/mcuboot/           # MCUboot bootloader
│   └── boot/zephyr/
│       └── boards/arm/nice_nano_v2_mcuboot/  # Custom MCUboot board
├── keys/                         # Signing keys (gitignored!)
│   ├── production-signing-key.pem      # ECDSA-P256 private key
│   └── production-signing-key-pub.pem  # Public key
└── modules/9p4z/                 # 9P library (via west)
```

## Flash Partition Layout (1MB)

| Region     | Address  | Size  | Purpose                    |
|------------|----------|-------|----------------------------|
| MCUboot    | 0x00000  | 48KB  | Bootloader                 |
| Scratch    | 0x0C000  | 8KB   | Swap operations            |
| Slot0      | 0x0E000  | 400KB | Primary app (ZMK runs here)|
| Slot1      | 0x72000  | 400KB | Secondary slot (DFU target)|
| Coredump   | 0xD6000  | 136KB | Memfault coredumps         |
| Storage    | 0xF8000  | 32KB  | NVS/Settings               |

## Build Steps

### 1. Build MCUboot Bootloader

```bash
cd /mnt2/src/zmk/bootloader/mcuboot/boot/zephyr

# Clean build with production signing key
rm -rf build
west build -p -b nice_nano_v2_mcuboot -- \
  -DBOARD_ROOT=/mnt2/src/zmk/bootloader/mcuboot/boot/zephyr \
  -DCONFIG_BOOT_SIGNATURE_KEY_FILE=\"/mnt2/src/zmk/keys/production-signing-key.pem\"
```

Output: `build/zephyr/zephyr.hex` (~21KB)

### 2. Build ZMK Application

```bash
cd /mnt2/src/zmk/app

west build -p -b nice_nano_v2 -- -DSHIELD=chocv
```

Output: `build/zephyr/zmk.hex` (~304KB)

### 3. Sign Application

```bash
west sign -t imgtool \
  -B build/zephyr/zmk-signed.bin \
  -H build/zephyr/zmk-signed.hex \
  -- --key /mnt2/src/zmk/keys/production-signing-key.pem --version 1.0.0
```

### 4. Merge MCUboot + Signed App

```bash
mergehex -m \
  /mnt2/src/zmk/bootloader/mcuboot/boot/zephyr/build/zephyr/zephyr.hex \
  /mnt2/src/zmk/app/build/zephyr/zmk-signed.hex \
  -o /mnt2/src/zmk/app/build/mcuboot_zmk_merged.hex
```

### 5. Flash via SWD

```bash
# Full chip erase + program (first time or recovery)
nrfjprog --program /mnt2/src/zmk/app/build/mcuboot_zmk_merged.hex \
  --chiperase --verify --reset

# Or just the app (if MCUboot unchanged)
nrfjprog --program /mnt2/src/zmk/app/build/zephyr/zmk-signed.hex \
  --sectorerase --verify --reset
```

## Quick Rebuild Script

```bash
#!/bin/bash
set -e
cd /mnt2/src/zmk && source .venv/bin/activate

# Rebuild app
cd app && west build -b nice_nano_v2 -- -DSHIELD=chocv

# Sign
west sign -t imgtool \
  -B build/zephyr/zmk-signed.bin \
  -H build/zephyr/zmk-signed.hex \
  -- --key /mnt2/src/zmk/keys/production-signing-key.pem --version 1.0.0

# Merge
mergehex -m \
  /mnt2/src/zmk/bootloader/mcuboot/boot/zephyr/build/zephyr/zephyr.hex \
  build/zephyr/zmk-signed.hex \
  -o build/mcuboot_zmk_merged.hex

# Flash
nrfjprog --program build/mcuboot_zmk_merged.hex --chiperase --verify --reset
```

## DFU Over 9P

Once running, firmware can be updated via the 9P `/dev/firmware` endpoint:

```bash
# From client (after mounting 9P):
cat new-firmware.bin > /mnt/9p/dev/firmware
# Reboot keyboard to apply
```

## Signing Key Management

**CRITICAL**: The `keys/` directory is gitignored. Back up your production
signing key securely! If lost, you cannot sign new firmware that the
bootloader will accept.

### Key Generation

```bash
# Generate new ECDSA-P256 key pair
openssl ecparam -name prime256v1 -genkey -noout -out keys/production-signing-key.pem
openssl ec -in keys/production-signing-key.pem -pubout -out keys/production-signing-key-pub.pem
```

### Using Same Key for ESP32

The same ECDSA-P256 key can be used for ESP32 MCUboot builds:

```bash
west build -b <esp32_board> -- \
  -DCONFIG_BOOT_SIGNATURE_KEY_FILE=\"/mnt2/src/zmk/keys/production-signing-key.pem\"
```

## Troubleshooting

### USB CDC Not Enumerating

Ensure `CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y` is in chocv.conf.
We disabled ZMK_USB (no HID), so USB must be auto-initialized.

### BLE Not Advertising

Check MCUboot is present and app is properly signed:
```bash
# Full reflash
nrfjprog --program mcuboot_zmk_merged.hex --chiperase --verify --reset
```

### Signature Mismatch

If MCUboot rejects the app (no boot), ensure:
1. MCUboot was built with the same key used to sign the app
2. The key path in both builds is identical
3. Both use ECDSA-P256 (not RSA)

## Version History

- 2025-12-31: Initial production build with ECDSA-P256 signing
- Thread pool for concurrent 9P operations
- DFU support via /dev/firmware
