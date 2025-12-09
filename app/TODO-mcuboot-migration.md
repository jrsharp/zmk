# MCUboot Migration Plan for nice_nano + Memfault

## Background

The Adafruit nRF52 bootloader clears RAM on reset, which prevents `.noinit` RAM-backed
coredumps from surviving reboots. Migrating to MCUboot gives us control over RAM
preservation and aligns with ESP32 bootloader patterns.

## Immediate: Test Current Approach

- [ ] Flash current firmware with direct-flash coredump storage
- [ ] Test: `mflt test crash` → reboot → `mflt get_core`
- [ ] If coredump persists, we may not need MCUboot migration

## MCUboot Migration Steps

### 1. Hardware Setup
- [ ] Solder SWD leads to nice_nano v2 bottom pads:
  - SWDIO, SWCLK, GND, 3.3V, RESET

### 2. Build MCUboot
- [ ] Build MCUboot for nice_nano with USB-CDC serial recovery
- [ ] Key Kconfig options:
  ```kconfig
  CONFIG_MCUBOOT_SERIAL=y
  CONFIG_BOOT_SERIAL_CDC_ACM=y
  CONFIG_USB_DEVICE_STACK=y
  CONFIG_USB_CDC_ACM=y
  CONFIG_BOOT_SERIAL_ENTRANCE_GPIO=y
  ```

### 3. Flash MCUboot
- [ ] One-time SWD flash of MCUboot to nice_nano
- [ ] Verify USB-CDC recovery mode works

### 4. Update ZMK Configuration
- [ ] Add to chocv.conf:
  ```kconfig
  CONFIG_BOOTLOADER_MCUBOOT=y
  ```
- [ ] Update flash partitions in DTS for MCUboot layout:
  ```
  0x000000 - 0x010000: MCUboot (64KB)
  0x010000 - 0x080000: Slot 0 / App (448KB)
  0x080000 - 0x0F0000: Slot 1 / Staging (448KB)
  0x0F0000 - 0x0F8000: Coredump partition (32KB)
  0x0F8000 - 0x100000: Settings/NVS (32KB)
  ```

### 5. Switch Memfault to Two-Stage Coredump
- [ ] Now that `.noinit` survives MCUboot reboot:
  - Fault handler: quick copy to `.noinit` RAM
  - Early boot: copy from `.noinit` RAM to flash
  - Clear `.noinit` RAM
- [ ] This is the canonical safe approach

### 6. Optional: tinyuf2 Integration
- [ ] Explore tinyuf2 for drag-and-drop UF2 experience
- [ ] Would restore the convenience of Adafruit bootloader

## DFU Workflow (After Migration)

```bash
# Enter bootloader (button hold or magic command)
# MCUboot exposes USB-CDC serial port
mcumgr --conntype serial --connstring /dev/ttyACM0 image upload build/zephyr/zephyr.signed.bin
# Boot into new image
```

## References

- Memfault noinit article: https://interrupt.memfault.com/blog/noinit-memory
- MCUboot docs: https://docs.mcuboot.com/
- Zephyr MCUboot integration: https://docs.zephyrproject.org/latest/services/device_mgmt/mcuboot.html
