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

CSV on UART0 @ 115200 (for debug alongside the Zigbee stream):

```
ts_ms,F1_415,F2_445,F3_480,F4_515,F5_555,F6_590,F7_630,F8_680,clear,nir,lux_est,zcl_mv
```

`lux_est` is the rough illuminance estimate pushed into the Zigbee `MeasuredValue` attribute; `zcl_mv` is the ZCL-encoded value (`10000·log10(lux)+1`) that ZHA actually sees.

## Calibration TODO

The current lux estimate is a single-point calibration on the clear channel (1 count ≈ 8 lux at 512× gain, ~281 ms integration) that I eyeballed against typical indoor lighting. For anything past "ballpark" accuracy, replace `clear_counts_to_lux()` in `main/main.c` with a photopic-weighted sum of F2…F8 and a proper reference-meter calibration.
