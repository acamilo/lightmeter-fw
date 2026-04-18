# lightmeter

10-channel spectral light meter firmware for the Adafruit AS7341 (PID 4698) driven by an ESP32-H2-DevKitM-1 (PID 5715) over STEMMA QT / I²C. The H2 acts as a Zigbee end-device and reports estimated illuminance to any ZHA-compatible coordinator (Home Assistant ZHA, Zigbee2MQTT, etc.) using the standard Illuminance Measurement cluster (0x0400).

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

## Pairing to Home Assistant (ZHA)

1. In Home Assistant: **Settings → Devices → ZHA → Add device**.
2. Power-cycle or reset the lightmeter. It boots factory-new, finds the network, and joins automatically. No install code needed (`install_code_policy = false`).
3. The device shows up as an Illuminance sensor on endpoint 1. The entity reports in lux and updates every 2 s.

If it doesn't find the network: ZHA must be in permit-join mode, and the coordinator channel must match. The firmware scans all 802.15.4 channels (`ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK`).

## Output

CSV on UART0 @ 115200 (for debug alongside the Zigbee stream):

```
ts_ms,F1_415,F2_445,F3_480,F4_515,F5_555,F6_590,F7_630,F8_680,clear,nir,lux_est,zcl_mv
```

`lux_est` is the rough illuminance estimate pushed into the Zigbee `MeasuredValue` attribute; `zcl_mv` is the ZCL-encoded value (`10000·log10(lux)+1`) that ZHA actually sees.

## Calibration TODO

The current lux estimate is a single-point calibration on the clear channel (1 count ≈ 8 lux at 512× gain, ~281 ms integration) that I eyeballed against typical indoor lighting. For anything past "ballpark" accuracy, replace `clear_counts_to_lux()` in `main/main.c` with a photopic-weighted sum of F2…F8 and a proper reference-meter calibration.
