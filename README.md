# lightmeter

10-channel spectral light meter firmware for the Adafruit AS7341 (PID 4698) driven by an ESP32-H2-DevKitM-1 (PID 5715) over STEMMA QT / I²C. Written against ESP-IDF (not Arduino) so `esp-zigbee-sdk` on the H2's 802.15.4 radio remains a first-class option for a future networked build.

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

## Output

CSV on UART0 @ 115200:

```
ts_ms,F1_415,F2_445,F3_480,F4_515,F5_555,F6_590,F7_630,F8_680,clear,nir
```

One line per spectral read. Sample rate depends on the integration time you configure (`atime`, `astep`); at the defaults in `main.c` (512× gain, ~281 ms integration) the stream runs ~1.5 Hz.
