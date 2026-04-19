# lightmeter

10-channel spectral light meter firmware for the Adafruit AS7341 (PID 4698) driven by an ESP32-H2-DevKitM-1 (PID 5715) over STEMMA QT / I²C. The H2 acts as a Zigbee end-device with **13 endpoints** — 11 Analog Input (F1..F8 per-band PPFD, PAR total, photopic lux, NIR) and 2 Binary Input (mains flicker detected, spectral saturation). ZHA auto-discovers each endpoint as its own sensor / binary_sensor entity.

## Build

ESP-IDF v5.3 or newer, target `esp32h2`:

```
idf.py set-target esp32h2
idf.py build
idf.py -p <serial port> flash monitor
```

The sensor driver is pulled automatically from the ESP Component Registry (`k0i05/esp_as7341`).

## Wiring

STEMMA QT cable (PID 4209) from the sensor's Qwiic port into the H2 header:

| AS7341 | ESP32-H2 |
|---|---|
| VIN | 3V3 |
| GND | GND |
| SDA | any free GPIO |
| SCL | any free GPIO |

On boot the firmware scans a list of candidate I²C pin pairs (`{12,22}`, `{4,5}`, `{1,0}`, `{10,11}`, `{2,3}`) in both polarities until the sensor ACKs at 0x39. If you wire to a pair outside that list, add it to `candidate_pairs[]` in `main/main.c`.

## Endpoints exposed to ZHA

| Endpoint | Cluster | Description (ZCL attr 0x001C) | Unit |
|---:|---|---|---|
| 1 | Analog Input | `F1 415nm PPFD umol/m2/s` | µmol/m²/s |
| 2 | Analog Input | `F2 445nm PPFD umol/m2/s` | µmol/m²/s |
| 3 | Analog Input | `F3 480nm PPFD umol/m2/s` | µmol/m²/s |
| 4 | Analog Input | `F4 515nm PPFD umol/m2/s` | µmol/m²/s |
| 5 | Analog Input | `F5 555nm PPFD umol/m2/s` | µmol/m²/s |
| 6 | Analog Input | `F6 590nm PPFD umol/m2/s` | µmol/m²/s |
| 7 | Analog Input | `F7 630nm PPFD umol/m2/s` | µmol/m²/s |
| 8 | Analog Input | `F8 680nm PPFD umol/m2/s` | µmol/m²/s |
| 9 | Analog Input | `PAR total PPFD umol/m2/s` | µmol/m²/s (sum of F1..F8) |
| 10 | Analog Input | `Illuminance lux photopic` | lux |
| 11 | Analog Input | `NIR 910nm PFD umol/m2/s` | µmol/m²/s |
| 12 | **Binary Input** | `Flicker 100/120Hz detected` | bool |
| 13 | **Binary Input** | `Spectral channel saturated` | bool |

Every endpoint is advertised as an HA Simple Sensor (device ID 0x000C) with Basic + Identify + either Analog Input (0x000C) or Binary Input (0x000F). ZHA's `EngineeringUnits` slot on the analog endpoints is set to `no_units` (95) since ZCL's unit enum has no entry for µmol/m²/s or lux; the Description attribute carries the real unit string. To pretty up the unit label on the HA entity, either (a) customize the unit per-entity in HA, or (b) ship a `zha-device-handlers` quirk that overrides display units.

**Flicker (EP 12)** goes `true` only when the AS7341's flicker engine locks onto 100 Hz or 120 Hz (mains-frequency light driver artifacts). `UNKNOWN` / `INVALID` states stay `false` — they mean "not locked yet," not "no flicker." Useful for grow-light QA.

**Saturation (EP 13)** goes `true` if any of the spectral or flicker-detect channels saturate (analog or digital) on the most recent read — i.e., your spectral data is capped and therefore lies about the real light level. Treat any non-zero reading as a hint to drop gain or shorten integration.

## Pairing to Home Assistant (ZHA)

1. In Home Assistant: **Settings → Devices → ZHA → Add device**.
2. Power-cycle or reset the lightmeter. It boots factory-new, scans all 802.15.4 channels, and joins automatically. No install code needed.
3. Eleven `sensor.*` entities plus two `binary_sensor.*` entities appear under one device, one per endpoint. All update every 2 s; the coordinator also gets a heartbeat every 60 s via configured attribute reporting.

## Output

CSV on UART0 @ 115200 mirrors the Zigbee data — one row per read at ~0.5 Hz, same column order as the endpoint table above:

```
ts_ms,F1 415nm PPFD umol/m2/s,F2 445nm PPFD umol/m2/s, ... ,PAR total PPFD umol/m2/s,Illuminance lux photopic
```

Useful for sanity-checking without pairing to a ZHA coordinator.

## Over-the-air updates

The device advertises the standard **OTA Upgrade cluster (0x0019)** as a client on endpoint 1, with dual-slot `ota_0`/`ota_1` partitions and bootloader rollback enabled. ZHA, Zigbee2MQTT, and any other ZCL OTA server can push new firmware without a USB cable.

**Identity** (must match between the device and the `.ota` file):

| Field | Value |
|---|---|
| Manufacturer code | `0x1289` |
| Image type | `0x0001` |
| Current file version | `0x00000001` (bump in `main/main.c` each release) |

### Build and package an update

```
idf.py build
scripts/make_ota.py build/lightmeter.bin lightmeter-v2.ota \
    --manufacturer 0x1289 --image-type 0x0001 --version 0x00000002
```

`make_ota.py` wraps the ESP-IDF `.bin` in the ZCL OTA Upgrade File format (header + `Upgrade Image` sub-element). Bump `--version` for every release — the coordinator serves only images strictly newer than what the device reports.

### Deploying via ZHA

1. Place `lightmeter-v2.ota` in `<HA config>/zigbee_ota/` (or wherever your `zha.configuration.ota_providers` points).
2. Restart HA or reload the ZHA integration.
3. ZHA announces the new image; the device downloads over the air and reboots into it. Expect **15–30 minutes** on a healthy mesh for a ~500 KB image.

### Deploying via Zigbee2MQTT

1. Drop the `.ota` into the path set by the `ota.image_block_response_delay` / `ota.custom_files` config (see z2m OTA docs).
2. Trigger an OTA check from the z2m UI.

### Rollback

If the new image fails to rejoin the network within a reasonable time, the bootloader reverts to the previous image on the next reset (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A bricked update un-bricks itself on next power cycle. Once the new firmware rejoins successfully, it calls `esp_ota_mark_app_valid_cancel_rollback()` and becomes the permanent image.

### First-time flash

The factory-flashed image must come via USB — partition layout changed from single `factory` to dual OTA slots. After that, OTA handles everything.

## Calibration TODO

Per-band responsivity coefficients (`responsivity_basic[]` in `main/main.c`) are AS7341 datasheet-typical values normalized into the k0i05 basic-counts domain — expect accuracy within a factor of ~2. For anything better, single-point-calibrate each band against a reference meter (Apogee MQ-500 / LI-COR LI-250 / similar) under a known PAR source, then scale each `responsivity_basic[i]` by `(firmware_umol / reference_umol)`.

The photopic lux channel (EP 10) uses CIE 1931 V(λ) weights sampled at the AS7341 band centers, with each band treated as a delta at its center wavelength — approximate, but sufficient for cross-checking PPFD channels against an ordinary lux meter.
