# MCUboot + Memfault Integration TODO

## Status: Ready to Flash

### Completed
- [x] Test current direct-flash coredump approach (failed - bootloader clears RAM)
- [x] Build MCUboot for nice_nano with USB-CDC serial recovery
- [x] Generate ECDSA-P256 signing key for MCUboot
- [x] Update ZMK/chocv config for MCUboot compatibility
- [x] Update flash partitions in DTS for MCUboot layout
- [x] Build and sign ZMK firmware with MCUboot image format

### In Progress
- [ ] Wire SWD from nRF5340-DK P20 header to nice_nano_v2

### Pending
- [ ] Flash MCUboot + ZMK via SWD (one-time operation)
- [ ] Test MCUboot boot + USB DFU
- [ ] Verify Memfault coredumps persist through reset
- [ ] (Optional) Switch Memfault to RAM->Flash two-stage coredump

---

## Build Artifacts

| File | Purpose |
|------|---------|
| `bootloader/mcuboot/boot/zephyr/build/zephyr/zephyr.hex` | MCUboot bootloader (48KB) |
| `app/build/zephyr/zmk.signed.bin` | Signed ZMK firmware (289KB) |
| `app/build/zephyr/zmk.signed.hex` | Signed ZMK (hex format) |
| **`app/build/mcuboot_zmk_merged.hex`** | **Complete flash image for SWD** |
| `keys/nice_nano_ecdsa_p256.pem` | ECDSA-P256 signing key |

---

## Flash Layout (1MB)

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| MCUboot | 0x00000 | 48KB | Bootloader |
| Scratch | 0x0C000 | 8KB | Swap operations |
| App (slot0) | 0x0E000 | 904KB | ZMK firmware |
| Coredump | 0xF0000 | 32KB | Memfault coredumps |
| Storage | 0xF8000 | 32KB | NVS/Settings |

---

## Wiring: nRF5340-DK P20 to nice_nano_v2

```
P20 Debug Out          nice_nano_v2
─────────────          ────────────
Pin 2 (SWDIO)  ──────► SWDIO pad
Pin 4 (SWDCLK) ──────► SWCLK pad
Pin 3 (GND)    ──────► GND
Pin 10 (RESET) ──────► RST (optional)
Pin 1 (VTG)    ──────► 3.3V (optional, for voltage sense)
```

---

## Flash Commands

```bash
# Activate virtualenv
cd /mnt2/src/zmk && source .venv/bin/activate

# Verify J-Link connection
nrfjprog --ids

# Flash everything (one command)
nrfjprog --program app/build/mcuboot_zmk_merged.hex --sectorerase --verify --reset

# Or flash separately:
# nrfjprog --program bootloader/mcuboot/boot/zephyr/build/zephyr/zephyr.hex --sectorerase
# nrfjprog --program app/build/zephyr/zmk.signed.hex --sectorerase --verify --reset
```

---

## After Flashing: USB DFU Updates

MCUboot waits 3 seconds on each boot for DFU commands:

```bash
# Install mcumgr if needed
go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest

# Upload new firmware over USB
mcumgr --conntype=serial --connstring="dev=/dev/ttyACM0,baud=115200" image upload app/build/zephyr/zmk.signed.bin
```

---

## Rebuild Commands

```bash
cd /mnt2/src/zmk && source .venv/bin/activate

# Rebuild MCUboot (if needed)
cd bootloader/mcuboot/boot/zephyr
west build -p -b nice_nano_v2_mcuboot -- -DBOARD_ROOT=/mnt2/src/zmk/bootloader/mcuboot/boot/zephyr

# Rebuild ZMK
cd /mnt2/src/zmk/app
west build -p -b nice_nano_v2 -- -DSHIELD=chocv -DCONFIG_MCUBOOT_SIGNATURE_KEY_FILE=\"/mnt2/src/zmk/keys/nice_nano_ecdsa_p256.pem\"

# Recreate merged hex
cd /mnt2/src/zmk && source .venv/bin/activate
python3 -c "
from intelhex import IntelHex
mcuboot = IntelHex('bootloader/mcuboot/boot/zephyr/build/zephyr/zephyr.hex')
app = IntelHex('app/build/zephyr/zmk.signed.hex')
merged = IntelHex()
merged.merge(mcuboot, overlap='ignore')
merged.merge(app, overlap='ignore')
merged.write_hex_file('app/build/mcuboot_zmk_merged.hex')
print('Merged hex created')
"
```

---

## Key Files Modified

- `app/boards/shields/chocv/chocv.conf` - Added `CONFIG_BOOTLOADER_MCUBOOT=y`
- `app/boards/shields/chocv/chocv.overlay` - MCUboot flash partition layout
- `app/src/memfault_flash_coredump.c` - Uses `coredump_partition`
- `bootloader/mcuboot/boot/zephyr/boards/arm/nice_nano_v2_mcuboot/` - Custom MCUboot board
- `bootloader/mcuboot/boot/zephyr/boards/nice_nano_v2_mcuboot.conf` - MCUboot config

---

## Why MCUboot?

The Adafruit nRF52 bootloader (stock on nice_nano) clears RAM on reset, wiping `.noinit` sections. MCUboot does NOT clear RAM, so Memfault coredumps stored in `.noinit` RAM will survive reboots and can be uploaded after crash recovery.
