# lightmeter

10-channel spectral light meter firmware for the Adafruit AS7341 (PID 4698) driven by an ESP32-H2-DevKitM-1 (PID 5715) over STEMMA QT / I²C. The H2 acts as a Zigbee end-device with **10 endpoints**, each exposing one Analog Input cluster so Home Assistant's ZHA integration auto-discovers them as ten separate sensor entities — F1..F8 per-band PPFD, PAR total, and photopic lux.

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

| Endpoint | Description (ZCL attr 0x001C) | Unit |
|---:|---|---|
| 1 | `F1 415nm PPFD umol/m2/s` | µmol/m²/s |
| 2 | `F2 445nm PPFD umol/m2/s` | µmol/m²/s |
| 3 | `F3 480nm PPFD umol/m2/s` | µmol/m²/s |
| 4 | `F4 515nm PPFD umol/m2/s` | µmol/m²/s |
| 5 | `F5 555nm PPFD umol/m2/s` | µmol/m²/s |
| 6 | `F6 590nm PPFD umol/m2/s` | µmol/m²/s |
| 7 | `F7 630nm PPFD umol/m2/s` | µmol/m²/s |
| 8 | `F8 680nm PPFD umol/m2/s` | µmol/m²/s |
| 9 | `PAR total PPFD umol/m2/s` | µmol/m²/s (sum of F1..F8) |
| 10 | `Illuminance lux photopic` | lux |

Each endpoint is advertised as an HA Simple Sensor (device ID 0x000C) with Basic + Identify + Analog Input (0x000C) clusters. ZHA's `EngineeringUnits` slot is set to `no_units` (95) since ZCL's unit enum has no entry for µmol/m²/s or lux; the Description attribute carries the human-readable unit string. To pretty up the unit label on the HA entity, either (a) customize the unit per-entity in HA, or (b) ship a `zha-device-handlers` quirk that overrides display units.

## Pairing to Home Assistant (ZHA)

1. In Home Assistant: **Settings → Devices → ZHA → Add device**.
2. Power-cycle or reset the lightmeter. It boots factory-new, scans all 802.15.4 channels, and joins automatically. No install code needed.
3. Ten `sensor.*` entities appear under one device, one per endpoint, updated every 2 s.

## Output

CSV on UART0 @ 115200 mirrors the Zigbee data — one row per read at ~0.5 Hz, same column order as the endpoint table above:

```
ts_ms,F1 415nm PPFD umol/m2/s,F2 445nm PPFD umol/m2/s, ... ,PAR total PPFD umol/m2/s,Illuminance lux photopic
```

Useful for sanity-checking without pairing to a ZHA coordinator.

## Calibration TODO

Per-band responsivity coefficients (`responsivity_basic[]` in `main/main.c`) are AS7341 datasheet-typical values normalized into the k0i05 basic-counts domain — expect accuracy within a factor of ~2. For anything better, single-point-calibrate each band against a reference meter (Apogee MQ-500 / LI-COR LI-250 / similar) under a known PAR source, then scale each `responsivity_basic[i]` by `(firmware_umol / reference_umol)`.

The photopic lux channel (EP 10) uses CIE 1931 V(λ) weights sampled at the AS7341 band centers, with each band treated as a delta at its center wavelength — approximate, but sufficient for cross-checking PPFD channels against an ordinary lux meter.
