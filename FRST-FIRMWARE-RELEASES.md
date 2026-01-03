# FRST Keyboard Firmware - Release Management

## Current State

### Version Sources
| Source | Value | Purpose |
|--------|-------|---------|
| `app/VERSION` | 0.3.0 | Upstream ZMK version |
| `app/src/memfault_platform.c` | "0.1.0-dev" (hardcoded) | Memfault device info |
| MCUboot image header | Set at sign time | Bootloader version check |
| Git describe | `v0.2-101-g8c8a3eb1` | Build identification |

### Problem
We have multiple version sources that aren't synchronized. Need a single source of truth for FRST product firmware versions.

---

## Proposed Version Scheme

### FRST Firmware Versioning
```
FRST-<product>-<major>.<minor>.<patch>[-<prerelease>][+<build>]
```

Examples:
- `FRST-M2KB-0.1.0-beta.1+8c8a3eb` - Model 2 Keyboard, beta 1
- `FRST-M2KB-1.0.0` - Model 2 Keyboard, first stable release
- `FRST-M2KB-1.0.1+hotfix` - Patch release

### Version Components
| Component | Meaning |
|-----------|---------|
| Product | M2KB = Model 2 Keyboard |
| Major | Breaking changes (protocol, API) |
| Minor | New features, backward compatible |
| Patch | Bug fixes only |
| Prerelease | alpha, beta.1, beta.2, rc.1, etc. |
| Build | Git short hash for traceability |

---

## Implementation Plan

### 1. Create FRST Version File

**File: `app/boards/shields/chocv/VERSION`**
```
VERSION_MAJOR = 0
VERSION_MINOR = 1
VERSION_PATCH = 0
VERSION_PRERELEASE = beta.1
VERSION_PRODUCT = M2KB
```

### 2. Update Memfault Platform Config

**File: `app/src/memfault_platform.c`**
```c
#include <zephyr/kernel.h>
#include <app_version.h>
#include <memfault/components.h>

/* Build version string from app_version.h macros */
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

void memfault_platform_get_device_info(sMemfaultDeviceInfo *info)
{
    *info = (sMemfaultDeviceInfo) {
        .device_serial = "FRST-M2KB-001",  // TODO: derive from chip ID
        .software_type = "frst-m2kb-9p",
        .software_version = TOSTRING(APP_BUILD_VERSION),
        .hardware_version = "nice_nano_v2_r1",
    };
}
```

### 3. Expose Version via 9P

Add `/kbd/version` node that returns:
```
product FRST-M2KB
version 0.1.0-beta.1
build 8c8a3eb1
zephyr 3.5.0
zmk 0.3.0
mcuboot 1.0.0
```

### 4. Signing Script Integration

Update signing to use consistent version:
```bash
#!/bin/bash
# sign-release.sh
VERSION=$(cat app/boards/shields/chocv/VERSION | grep VERSION | ...)
GIT_HASH=$(git rev-parse --short HEAD)

west sign -t imgtool -d app/build -- \
    --key keys/production-signing-key.pem \
    --version ${VERSION}+${GIT_HASH}
```

---

## 9P Settings Interface Design

### Proposed Filesystem Structure
```
/kbd/
├── kbin              # [existing] Keypress stream (blocking read)
├── leds              # [existing] LED state byte
├── battery           # [existing] "percent millivolts\n"
├── ctl               # [existing] Control commands (reboot)
├── version           # [NEW] Firmware version info
├── dev/
│   ├── firmware      # [existing] DFU upload
│   └── mflt          # [existing] Memfault chunk export
└── settings/         # [NEW] Runtime configuration
    ├── debounce      # Debounce timing
    ├── repeat        # Key repeat settings (future)
    ├── idle          # Idle/sleep timeouts
    └── ble           # BLE parameters (future)
```

### Settings Node Behaviors

#### `/kbd/version` (read-only)
```
Read:
  product FRST-M2KB
  version 0.1.0-beta.1
  build 8c8a3eb1
  zephyr 3.5.0
  uptime 3600
```

#### `/kbd/settings/debounce` (read-write)
```
Read:
  press_ms 10
  release_ms 10
  scan_period_ms 1

Write:
  echo "press_ms 5" > /kbd/settings/debounce
  echo "release_ms 5" > /kbd/settings/debounce
```

Implementation notes:
- Changes take effect immediately
- Persist to NVS on write (debounced save)
- Validate ranges before applying

#### `/kbd/settings/idle` (read-write)
```
Read:
  idle_timeout_ms 30000
  sleep_timeout_ms 300000
  sleep_enabled true

Write:
  echo "idle_timeout_ms 60000" > /kbd/settings/idle
  echo "sleep_enabled false" > /kbd/settings/idle
```

#### `/kbd/settings/ble` (read-write, future)
```
Read:
  min_interval 24
  max_interval 40
  latency 30
  tx_power 0

Write:
  echo "tx_power 8" > /kbd/settings/ble
```

Note: BLE parameter changes require disconnect/reconnect to take effect.

---

## ZMK Studio Comparison

| Feature | ZMK Studio | 9P Settings |
|---------|------------|-------------|
| **Transport** | Protobuf RPC over BLE GATT/UART | 9P over BLE L2CAP |
| **Keymap editing** | Full layer/binding editing | Future (complex) |
| **RGB settings** | Yes | N/A (no RGB on chocv) |
| **Debounce** | No | Yes (proposed) |
| **Idle timeouts** | No | Yes (proposed) |
| **BLE params** | No | Yes (proposed) |
| **Security** | Lock/unlock mechanism | Unix permissions (future) |
| **Client app** | ZMK Studio web app | FRST Terminal |

### Why 9P vs ZMK Studio?

1. **Already have 9P** - Our transport layer is established
2. **Simpler protocol** - Text-based ctl files vs protobuf
3. **Terminal integration** - FRST Terminal already speaks 9P
4. **Flexibility** - Can expose anything as a file
5. **Unix philosophy** - Compose with standard tools

### What ZMK Studio Has That We Don't Need (Yet)

- Full keymap editing (complex, maybe later)
- Layer reordering (keymap is compiled for now)
- Behavior metadata (RPC-heavy)
- Multi-device coordination (split keyboards)

---

## Public Beta Release Checklist

### Beta 1 (v0.1.0-beta.1) - Minimum Viable

- [ ] Version tracking implemented
- [ ] `/kbd/version` node
- [ ] `/kbd/settings/debounce` node
- [ ] Power optimizations (DONE in current commit)
- [ ] Deep sleep working (DONE in current commit)
- [ ] DFU tested end-to-end
- [ ] Memfault export tested
- [ ] Build reproducible from git tag

### Beta 2 (v0.1.0-beta.2) - Polish

- [ ] `/kbd/settings/idle` node
- [ ] Settings persistence to NVS
- [ ] PPK2 power measurements documented
- [ ] Battery life estimates

### Release Candidate (v0.1.0-rc.1)

- [ ] All known bugs fixed
- [ ] Documentation complete
- [ ] FRST Terminal compatibility verified
- [ ] 1 week soak test passed

### Stable (v1.0.0)

- [ ] Public feedback addressed
- [ ] Production signing key secured
- [ ] Release notes written

---

## Build & Release Commands

### Development Build
```bash
west build -p -s app -b nice_nano_v2 -- -DSHIELD=chocv
west sign -t imgtool -d app/build -- \
    --key keys/production-signing-key.pem \
    --version 0.1.0-dev+$(git rev-parse --short HEAD)
```

### Tagged Release Build
```bash
git tag -a v0.1.0-beta.1 -m "First public beta"
git push origin v0.1.0-beta.1

west build -p -s app -b nice_nano_v2 -- -DSHIELD=chocv
west sign -t imgtool -d app/build -- \
    --key keys/production-signing-key.pem \
    --version 0.1.0-beta.1

# Archive release artifacts
mkdir -p releases/v0.1.0-beta.1
cp app/build/zephyr/zephyr.signed.{hex,bin} releases/v0.1.0-beta.1/
cp app/build/zephyr/zmk.uf2 releases/v0.1.0-beta.1/
sha256sum releases/v0.1.0-beta.1/* > releases/v0.1.0-beta.1/SHA256SUMS
```

---

## Implementation Priority

### Phase 1: Version & Info (Beta 1)
1. Create `/kbd/version` node
2. Update `memfault_platform.c` with dynamic version
3. Create signing script
4. Tag first beta release

### Phase 2: Basic Settings (Beta 2)
1. Add `/kbd/settings/` directory structure
2. Implement debounce settings node
3. Implement idle timeout settings node
4. Add NVS persistence for settings

### Phase 3: Advanced Settings (Post-Beta)
1. BLE parameter tuning
2. Key repeat settings (if we implement key repeat)
3. Consider keymap snippets (per-key overrides?)

---

## Questions to Resolve

1. **Unique device serial**: Derive from nRF chip ID? User-settable?
2. **Settings validation**: Hard limits or warn-only?
3. **Factory reset**: Add `factory_reset` to `/kbd/ctl`?
4. **Multiple profiles**: Support different setting profiles?
