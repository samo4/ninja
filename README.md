# Ninja

Originals:

https://github.com/samo4/shadowguard
https://github.com/samo4/shadow_playground
https://github.com/samo4/ShadowGuardGNSS
https://github.com/samo4/lis2dtw12

This repository is a fork of the [nRF Asset Tracker Template](https://github.com/nrfconnect/Asset-Tracker-Template), adapted with significant trimming of features.

## Core features / TODO

- [ ] connect to LTE
- [ ] disconnect from LTE
- [ ] go to deep sleep (ARMED)
- [ ] acquire GPS lock
  - [ ] [A-GPS](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/app/src/modules/location/location.c)
- [ ] validate current usage on shadow board < 40uA
- [ ] measure battery voltage
- [ ] over the air update (FOTA)
- [ ] detect movement
- [ ] handle "device in use, ignore movement" situation

## Get started

### Prerequisites

```bash
nrfutil install sdk-manager
nrfutil sdk-manager install v3.4.0
```

### Clone and build

```bash
# Initialize the west workspace using this repo as the manifest
west init -m https://github.com/samo4/ninja.git --mr main workspace

cd workspace

# Pull in the nRF Connect SDK modules
west update

# Build for your target
cd app

# Build, flash, and verify — default runs all three:
./run.sh

# Or run steps individually:
./run.sh build     # compile only
./run.sh flash     # flash only (skips build)
./run.sh check     # check for HardFault via J-Link


# see console
tio -b 115200 /dev/ttyACM0
# or RTT
JLinkRTTLogger -device nRF9160_xxAA -if SWD -speed 4000 -RTTAddress Auto
```

See the [Getting Started](docs/common/getting_started.md) guide for detailed instructions on flashing, connecting to nRF Cloud, and testing.

### Connecting to nRF Cloud (non-Nordic SIM) (deprecated)

If you are using a third-party SIM (not an nRF Cloud SIM), the device needs to be provisioned with credentials and registered on nRF Cloud manually.

```bash
pip3 install nrf-cloud-utils

# Get an API key from [nrfcloud.com](https://nrfcloud.com) → **Account** → **API Keys**.

# With the device powered on and connected to LTE, create a local certificate authority and provision credentials to the device over CoAP:

create_ca_cert
device_credentials_installer -d --ca *_ca.pem --ca-key *_prv.pem --coap --verify --id-imei --id-str nrf- -rtt

# Register the device with your nRF Cloud account:
nrf_cloud_onboard --api-key $API_KEY --csv onboard.csv
```

The device will authenticate and connect on the next provisioning attempt (up to 60 seconds).

### Updating modem firmware

Download the latest `mfw_nrf9160_*.zip` from [Nordic's nRF9160 page](https://www.nordicsemi.com/Products/nRF9160/Download).

```bash
nrfutil device list --traits modem,jlink
nrfutil device program --firmware mfw_nrf9160_1.3.7.zip --serial-number <SERIAL_NUMBER>
```

### Changes from upstream

| Change                                                                                                                                   | Files                                                                                                                |
| ---------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| **Switch to Thingy:91 (nRF9160)** — added board-specific config and overlay for the original Thingy:91 with nRF9160 SiP                  | `app/boards/thingy91_nrf9160_ns.conf`, `app/boards/thingy91_nrf9160_ns.overlay`                                      |
| **Remove MCUboot** — no OTA/bootloader needed; TF-M boots directly at 0x0, application at 0x10000                                        | `app/Kconfig.sysbuild`, `app/prj.conf`, `app/boards/thingy91_nrf9160_ns.conf`                                        |
| **Remove FOTA** — stripped firmware-over-the-air module, downloader, and all associated Kconfig                                          | `app/prj.conf`, `app/src/main.c` (FOTA includes, channel, states, handlers removed), `app/sysbuild/mcuboot/prj.conf` |
| **Remove Memfault** — stripped cloud crash-reporting and metrics entirely                                                                | `app/prj.conf` (all `CONFIG_MEMFAULT_*` removed)                                                                     |
| **Remove MQTT example** — deleted the MQTT cloud module example code                                                                     | `examples/modules/cloud/` (entire directory deleted)                                                                 |
| **Simplify sysbuild** — removed b0/NSIB `merged.hex` dependency; image merge only when b0 target exists                                  | `app/sysbuild.cmake`                                                                                                 |
| **Simplify flash layout** — reclaimed MCUboot, secondary-slot, and scratch partitions (968 KB for application code)                      | `app/boards/thingy91_nrf9160_ns.overlay` (partition overrides)                                                       |
| **SPU alignment** — non-secure start address aligned to nRF9160 SPU region boundary (32 KB)                                              | `app/boards/thingy91_nrf9160_ns.overlay`                                                                             |
| **Update SDK version** — from `v3.4.0-rc2` to stable `v3.4.0` (LTS)                                                                      | `west.yml`                                                                                                           |
| **Enable RTT console** — replaced UART (dead on Thingy:91 via nRF52840) with J-Link RTT                                                  | `app/boards/thingy91_nrf9160_ns.conf`                                                                                |
| **Fix FOTA flash page size** — replaced hardcoded `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE` with runtime flash API                         | `app/src/modules/fota/fota.c`                                                                                        |
| **Reduce MCUboot size** — minimized stack, removed serial recovery, disabled multithreading (attempted before removing MCUboot entirely) | `app/sysbuild/mcuboot/prj.conf`                                                                                      |

---

## Documentation

See original.
