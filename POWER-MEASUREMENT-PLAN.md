# Power Measurement Plan: 9P-over-L2CAP vs HOGP

## Objective

Quantify power consumption differences between:
1. **Standard ZMK with HOGP** (BLE HID over GATT)
2. **9P-over-L2CAP** with blocking-read model

Hypothesis: The blocking-read model allows deeper idle states and has lower protocol overhead, resulting in measurable power savings.

---

## Hardware Setup

### Equipment Required
- Nordic PPK2 (Power Profiler Kit 2)
- nice!nano v2 (test subject)
- nRF5340-DK or J-Link for SWD flashing
- Host device for BLE connection (ESP32 for 9P, computer/phone for HOGP)
- USB isolator (optional, to measure USB-disconnected power)

### PPK2 Wiring

The nice!nano v2 needs to be powered through the PPK2 for accurate measurements.

```
PPK2 Source Mode Wiring:
┌─────────────┐
│    PPK2     │
│             │
│  VOUT ──────┼──────> nice!nano VCC (bypass onboard LDO)
│  GND  ──────┼──────> nice!nano GND
│             │
│  (USB to PC │
│   for data) │
└─────────────┘
```

**Critical**: To measure true MCU current:
1. Remove battery from nice!nano
2. Connect PPK2 VOUT to the VCC pin (3.3V rail), NOT RAW/VBAT
3. Set PPK2 source voltage to 3.3V
4. This bypasses the nice!nano's onboard regulator

**Alternative** (measure full system including regulator):
1. Connect PPK2 to RAW pin (battery input)
2. Set PPK2 source voltage to 3.7-4.2V (simulating LiPo)
3. This measures total system draw including regulator efficiency

Recommendation: Do both - MCU-only shows software efficiency, full-system shows real-world battery drain.

---

## Firmware Builds

### Build 1: Baseline HOGP (Stock ZMK)

Create a minimal HOGP config for comparison:

```bash
# Create test config
mkdir -p app/boards/shields/chocv_hogp_test
```

**chocv_hogp_test.conf:**
```kconfig
# Standard ZMK BLE HID
CONFIG_ZMK_BLE=y
CONFIG_ZMK_USB=n

# Match our optimized BLE params for fair comparison
CONFIG_BT_PERIPHERAL_PREF_MIN_INT=24
CONFIG_BT_PERIPHERAL_PREF_MAX_INT=40
CONFIG_BT_PERIPHERAL_PREF_LATENCY=30
CONFIG_BT_PERIPHERAL_PREF_TIMEOUT=400

# DCDC enabled (same as 9P build)
CONFIG_BOARD_ENABLE_DCDC=y
CONFIG_BOARD_ENABLE_DCDC_HV=y

# Power management (same as 9P build)
CONFIG_PM=y
CONFIG_PM_DEVICE=y
CONFIG_ZMK_IDLE_TIMEOUT=30000
CONFIG_ZMK_SLEEP=y
CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=300000

# TX power (same as 9P build)
CONFIG_BT_CTLR_TX_PWR_0=y

# Minimal logging
CONFIG_LOG=n
CONFIG_SHELL=n
CONFIG_USB_DEVICE_STACK=n

# NFC pins as GPIO
CONFIG_NFCT_PINS_AS_GPIOS=y
```

Build command:
```bash
west build -p -s app -b nice_nano_v2 -- -DSHIELD=chocv_hogp_test
```

### Build 2: 9P-over-L2CAP (Current Build)

Use the existing chocv config with power optimizations.

For fair comparison, also create a "minimal" variant without USB/shell:
```kconfig
# Add to chocv.conf for measurement build:
CONFIG_USB_DEVICE_STACK=n
CONFIG_SHELL=n
CONFIG_LOG=n
```

Build command:
```bash
west build -p -s app -b nice_nano_v2 -- -DSHIELD=chocv
west sign -t imgtool -d app/build -- --key keys/production-signing-key.pem --version 1.2.0+0
```

### Build Matrix

| Build | BLE Profile | USB | Logging | Purpose |
|-------|-------------|-----|---------|---------|
| hogp-minimal | HOGP | Off | Off | Fair HOGP comparison |
| hogp-debug | HOGP | On | On | Debug/verify HOGP works |
| 9p-minimal | 9P/L2CAP | Off | Off | Fair 9P comparison |
| 9p-debug | 9P/L2CAP | On | On | Current dev build |

---

## Test Scenarios

### Test 1: Deep Sleep Current

**Purpose**: Verify system-off current is similar (sanity check)

**Procedure**:
1. Flash firmware
2. Connect BLE, then disconnect
3. Wait for deep sleep (5 min idle timeout)
4. Measure steady-state current for 60 seconds

**Expected**: <10µA for both builds (nRF52840 system-off spec)

**PPK2 Settings**:
- Sample rate: 100 kSps
- Duration: 60 seconds
- Export: average, min, max

---

### Test 2: Idle Connected Current

**Purpose**: Compare BLE connection maintenance overhead

**Procedure**:
1. Flash firmware
2. Establish BLE connection
3. No keypresses - let it idle
4. Measure for 5 minutes (before deep sleep triggers)

**Expected**:
- HOGP: ~200-500µA average (GATT notification ready state)
- 9P: Potentially lower (blocking on semaphore, no GATT state)

**PPK2 Settings**:
- Sample rate: 100 kSps
- Duration: 300 seconds (5 min)
- Export: average, waveform for radio pattern analysis

**Analysis**:
- Look at radio TX/RX spikes pattern
- Calculate duty cycle
- Compare average current

---

### Test 3: Active Typing Burst

**Purpose**: Compare data transmission efficiency

**Procedure**:
1. Flash firmware
2. Establish BLE connection
3. Type a standardized phrase: "The quick brown fox jumps over the lazy dog."
4. Use consistent typing speed (~60 WPM / 5 chars per second)
5. Record entire typing session

**Expected**:
- Similar peak currents (radio TX is radio TX)
- Possible difference in protocol overhead between packets
- 9P may batch scancodes more efficiently

**PPK2 Settings**:
- Sample rate: 100 kSps
- Trigger: manual start before typing
- Duration: 30 seconds
- Export: full waveform

**Analysis**:
- Total energy (µAh) for the typing session
- Packet count (count TX spikes)
- Per-character energy cost

---

### Test 4: Mixed Usage Simulation

**Purpose**: Real-world usage pattern

**Procedure**:
1. Flash firmware
2. Simulate 10-minute usage cycle:
   - 0:00-1:00: Active typing
   - 1:00-3:00: Idle (reading)
   - 3:00-3:30: Active typing
   - 3:30-6:00: Idle (reading)
   - 6:00-10:00: Idle (will enter deep sleep at 5:00 mark)
3. Record entire 10 minutes

**Expected**:
- See transition through all power states
- Calculate average current for "realistic" usage

**PPK2 Settings**:
- Sample rate: 10 kSps (lower rate for longer capture)
- Duration: 600 seconds (10 min)
- Export: average per second, full waveform

---

### Test 5: Wake-from-Sleep Latency

**Purpose**: Measure reconnection time/energy cost

**Procedure**:
1. Let keyboard enter deep sleep
2. Press a key, start PPK2 capture simultaneously
3. Record until BLE reconnection complete and keypress registers

**Expected**:
- ~1-3 seconds wake time
- Measure energy cost of wake + reconnect

**PPK2 Settings**:
- Sample rate: 100 kSps
- Trigger: current threshold (wake spike)
- Duration: 10 seconds
- Export: full waveform

---

## Data Collection Template

### Per-Test Recording

```
Date: ___________
Build: ___________
Test: ___________

PPK2 Settings:
- Source Voltage: _____ V
- Sample Rate: _____ kSps
- Duration: _____ s

Environmental:
- Room Temp: _____ °C
- BLE RSSI: _____ dBm (if measurable)

Results:
- Average Current: _____ µA
- Peak Current: _____ mA
- Min Current: _____ µA
- Total Charge: _____ µAh

Notes:
_____________________
```

### Summary Comparison Table

| Metric | HOGP | 9P/L2CAP | Delta | Notes |
|--------|------|----------|-------|-------|
| Deep sleep (µA) | | | | Should be equal |
| Idle connected (µA) | | | | Key comparison |
| Typing burst (µAh) | | | | Per-phrase energy |
| Mixed usage avg (µA) | | | | Real-world proxy |
| Wake energy (µAh) | | | | One-time cost |

---

## Analysis Goals

### Primary Questions

1. **Is 9P idle current lower than HOGP?**
   - If yes, validates the blocking-read hypothesis
   - Quantify the difference in µA and %

2. **Is per-keypress energy similar?**
   - Protocol overhead difference should show here
   - L2CAP CoC vs ATT/GATT

3. **What's the real-world battery life difference?**
   - Use mixed-usage data to project
   - nice!nano battery: ~110mAh typical
   - Calculate days of battery life for each

### Secondary Analysis

1. **Radio duty cycle comparison**
   - Analyze TX/RX spike patterns in waveforms
   - Are connection events handled differently?

2. **CPU active time**
   - Look at baseline between radio spikes
   - Does blocking read allow lower CPU idle current?

3. **Protocol efficiency**
   - Count packets for equivalent data
   - Calculate overhead per keypress

---

## Potential Confounding Factors

1. **BLE connection interval negotiation**
   - Host may negotiate different actual intervals
   - Log/verify actual connection params on both sides

2. **Different host devices**
   - ESP32 (9P) vs computer/phone (HOGP)
   - Try to use same host if possible, or document difference

3. **Firmware size/complexity**
   - 9P build has more code (DFU, Memfault, etc.)
   - Consider stripped-down builds for fair comparison

4. **Temperature**
   - nRF52840 current varies with temperature
   - Keep environment consistent or document

---

## File Outputs

Save all data to: `$ZMK/power-measurements/`

```
power-measurements/
├── raw/
│   ├── hogp-test1-deepsleep.ppk2
│   ├── hogp-test2-idle.ppk2
│   ├── 9p-test1-deepsleep.ppk2
│   └── ...
├── exports/
│   ├── hogp-test2-idle.csv
│   └── ...
├── analysis/
│   ├── comparison-charts.png
│   └── summary.md
└── builds/
    ├── hogp-minimal.hex
    └── 9p-minimal.hex
```

---

## Quick Start Checklist

- [ ] PPK2 connected and nRF Connect for Desktop installed
- [ ] nice!nano wired to PPK2 (VCC + GND)
- [ ] SWD debugger connected for flashing
- [ ] HOGP baseline build created and tested
- [ ] 9P build ready (current chocv)
- [ ] Host devices ready (ESP32 for 9P, computer for HOGP)
- [ ] Data collection spreadsheet/template ready
- [ ] `power-measurements/` directory created

---

## References

- [Nordic PPK2 User Guide](https://infocenter.nordicsemi.com/topic/ug_ppk2/UG/ppk/PPK_user_guide_Intro.html)
- [nRF52840 Power Optimization](https://infocenter.nordicsemi.com/topic/nan_042/APP/nan_power_optimization/intro.html)
- [BLE Connection Parameters](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf) (Apple's guidelines, good baseline)
- nice!nano v2 schematic (for understanding power path)
