# FRST Terminal: Keyboard Pairing Spec

Minimal spec for implementing 1:1 keyboard pairing on the ESP32 terminal.

## Overview

Each terminal pairs with exactly one keyboard. The keyboard's BLE MAC address is stored in NVS and used to filter connections.

## Keyboard Behavior (Already Implemented)

- Advertises as BLE peripheral with name: `FRST-M1KB-XXXX` or `FRST-M2KB-XXXX`
- `XXXX` = last 2 bytes of MAC in hex (e.g., `A3F2`)
- Uses stable public MAC address (no privacy rotation)
- Supports bonding for encrypted reconnects
- 9P service on L2CAP PSM `0x0081`
- Also advertises UUID `0x1001` for discovery

## Terminal Requirements

### 1. NVS Storage

Store paired keyboard MAC in NVS:

```
Namespace: "frst"
Key: "kb_mac"
Value: 6-byte MAC address (or empty/zeroed if unpaired)
```

### 2. Pairing Mode

When no keyboard is paired (or user triggers re-pair):

1. Scan for BLE devices advertising UUID `0x1001`
2. Filter for names matching pattern `FRST-M*KB-*`
3. Display discovered keyboards (name + MAC)
4. User selects one (or auto-select if only one found)
5. Store selected MAC in NVS
6. Initiate connection

### 3. Normal Boot

When keyboard MAC is stored:

1. Read MAC from NVS
2. Scan for device with matching MAC address
3. Connect only to that device (ignore others)
4. If not found within timeout: show "keyboard not found" status

### 4. Connection Flow

```
Terminal (BLE Central)              Keyboard (BLE Peripheral)
        |                                    |
        |--- Scan for UUID 0x1001 ---------->|
        |<-- Advertising: FRST-M1KB-A3F2 ----|
        |                                    |
        |--- Connect (filter by MAC) ------->|
        |<-- Connected ----------------------|
        |                                    |
        |--- Bond (if not already) --------->|
        |<-- Encryption established ---------|
        |                                    |
        |--- L2CAP Connect PSM 0x0081 ------>|
        |<-- L2CAP Channel established ------|
        |                                    |
        |--- 9P Tattach -------------------->|
        |<-- 9P Rattach ---------------------|
```

### 5. Reconnection

After bonding, reconnection should:
- Use stored bond info (skip pairing)
- Connect faster (known MAC, skip scan if possible)
- Re-establish L2CAP channel
- Resume 9P session

### 6. User Actions

Provide way to:
- **View paired keyboard**: Show name + MAC
- **Forget keyboard**: Clear NVS, return to pairing mode
- **Re-pair**: Scan for new keyboard, replace stored MAC

## Example Implementation

```c
// NVS read
uint8_t paired_mac[6];
nvs_get_blob(handle, "kb_mac", paired_mac, &len);

// Check if paired
bool is_paired = (len == 6 && !is_zero(paired_mac));

// During scan, filter by MAC
if (is_paired) {
    if (memcmp(scan_result->mac, paired_mac, 6) == 0) {
        // This is our keyboard - connect
    }
} else {
    // Pairing mode - show all FRST keyboards
    if (strncmp(scan_result->name, "FRST-M", 6) == 0) {
        // Add to discovered list
    }
}
```

## Notes

- Keyboard MAC is stable (public address from nRF chip)
- Bonding encrypts the L2CAP channel (keystrokes protected)
- Terminal is BLE central, keyboard is peripheral
- Only one keyboard can be paired at a time
