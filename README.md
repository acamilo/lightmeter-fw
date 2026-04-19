# lightmeter

A 10-channel spectral light meter that reports per-band photosynthetic photon flux (PPFD), total PAR, photopic illuminance, flicker detection, and spectral saturation over Zigbee — compatible with Home Assistant ZHA, Zigbee2MQTT, and any other ZCL coordinator.

Built on an Adafruit AS7341 sensor and an ESP32-H2-DevKitM-1. ESP-IDF firmware, no external cloud, OTA-capable once paired.

```
┌─────────────┐   I²C    ┌──────────────┐  802.15.4 (Zigbee 3.0)   ┌──────────┐
│   AS7341    ├─────────►│   ESP32-H2   │◄────────────────────────►│  ZHA /   │──► HA
│ (PID 4698)  │ 0x39     │ (PID 5715)   │                          │   z2m    │
└─────────────┘          └──────────────┘                          └──────────┘
```

## Bill of materials

| Part | SKU | Notes |
|---|---|---|
| ESP32-H2-DevKitM-1 (4 MB flash) | Adafruit 5715 | RISC-V single core @ 96 MHz. BT 5 LE + IEEE 802.15.4. **No Wi-Fi**. USB via CP2102N. |
| AS7341 10-channel spectral sensor | Adafruit 4698 | STEMMA QT / Qwiic. 8 visible bands (415–680 nm) + NIR (910 nm) + clear + flicker engine. |
| STEMMA QT → male header pigtail, 150 mm | Adafruit 4209 | Breadboard glue. |

Total material cost: ~$30.

## Wiring

| AS7341 | ESP32-H2 |
|---|---|
| VIN | 3V3 |
| GND | GND |
| SDA | any free GPIO (firmware auto-detects) |
| SCL | any free GPIO |

The firmware probes a list of candidate SDA/SCL pin pairs (`12/22`, `4/5`, `1/0`, `10/11`, `2/3`) in both polarities on boot and locks onto whichever one ACKs at 0x39. No menuconfig needed to match your wiring.

## Build

Prereqs: ESP-IDF v5.3+ (tested on 5.3.8), Python 3.10+.

```
git clone https://github.com/acamilo/lightmeter-fw
cd lightmeter-fw
idf.py set-target esp32h2
idf.py build
idf.py -p <serial port> flash monitor
```

The sensor driver (`k0i05/esp_as7341`) and Zigbee stack (`espressif/esp-zigbee-lib` + `esp-zboss-lib`) pull in automatically from the ESP Component Registry.

### CSV debug stream

Serial output at 115200 baud mirrors what goes over Zigbee, one line per sample:

```
ts_ms,F1 415nm PPFD,F2 445nm PPFD, ... ,PAR total,Illuminance lux,NIR 910nm PFD,flicker,saturated
1476,0.000,0.000,0.000,0.065,0.302,0.417,0.569,0.374,1.728,114.735,0.000,0,0
```

## Zigbee entities exposed

The device presents itself as a 13-endpoint ZHA device (`Espressif` / `lightmeter`, HA profile 0x0104, Simple Sensor 0x000C).

| EP | Cluster | Description | Unit |
|---:|---|---|---|
| 1 | AnalogInput + OTA (client) | F1 415 nm PPFD | µmol/m²/s |
| 2–8 | AnalogInput | F2 445 / F3 480 / F4 515 / F5 555 / F6 590 / F7 630 / F8 680 nm PPFD | µmol/m²/s |
| 9 | AnalogInput | PAR total (sum of F1..F8) | µmol/m²/s |
| 10 | AnalogInput | Illuminance (photopic-weighted V(λ)) | lux |
| 11 | AnalogInput | NIR 910 nm photon flux density | µmol/m²/s |
| 12 | BinaryInput | Flicker 100/120 Hz detected | bool |
| 13 | BinaryInput | Spectral channel saturated | bool |

Attribute reporting is configured at join-time: min 2 s, max 60 s, reportable change 0.1.

## Home Assistant (ZHA) integration

ZHA's generic auto-discovery can match the AnalogInput clusters but doesn't always surface them as HA entities on multi-endpoint devices. The **quirk** in `zha_quirk/acamilo_lightmeter.py` fixes that by explicitly declaring one sensor per endpoint.

### One-time HA setup

1. Copy `zha_quirk/acamilo_lightmeter.py` to `<HA config>/custom_zha_quirks/`.
2. Add to `configuration.yaml`:
   ```yaml
   zha:
     custom_quirks_path: /config/custom_zha_quirks
   ```
3. Restart HA Core.

### Pairing

1. In HA: Settings → Devices & Services → ZHA → **Add device** (60 s permit-join).
2. Power-cycle the lightmeter. Factory-new devices find the network and join automatically; no install code needed.
3. The device appears as `Espressif lightmeter` with 11 sensors + 2 diagnostic binary_sensors, one per endpoint.

### Unit labels

ZHA's unit system rejects arbitrary strings, so the µmol/m²/s channels appear without a default unit. To set it:

- Per entity, via the HA UI: click the entity → ⚙ Settings → set **Unit of measurement** to `µmol/m²/s`.

Or bulk-set all 10 via the HA REST API:

```
for ent in f1_415nm_ppfd f2_445nm_ppfd f3_480nm_ppfd f4_515nm_ppfd \
           f5_555nm_ppfd f6_590nm_ppfd f7_630nm_ppfd f8_680nm_ppfd \
           par_total nir_910nm_pfd; do
    curl -X POST -H "Authorization: Bearer <long-lived-token>" \
         -H "Content-Type: application/json" \
         -d '{"unit_of_measurement":"µmol/m²/s"}' \
         http://<ha>:8123/api/config/entity_registry/sensor.espressif_lightmeter_$ent
done
```

The lux channel (EP 10) gets `lx` automatically via the `UnitOfIlluminance.LUX` enum.

## Custom Lovelace card

`ha_card/lightmeter-card.js` is a vanilla web component that renders the spectrum as a coloured bar chart with PAR / lux headline numbers and flicker / saturation indicators. No dependencies, drops into HA directly.

Install:
1. Copy `ha_card/lightmeter-card.js` into `<HA config>/www/`.
2. Register as a Lovelace resource: Settings → Dashboards → ⋮ Resources → **Add**, URL `/local/lightmeter-card.js`, type **JavaScript Module**.
3. Restart HA (or clear the browser's cache for the dashboard).
4. On a dashboard, add a **Manual** card with:
   ```yaml
   type: custom:lightmeter-card
   ```

Override keys if your device name differs (`title`, `entity_base`, `binary_base`).

## Zigbee2MQTT

A standards-compliant device — z2m picks it up, but its default `numeric` converter gives generic names. For pretty entities, a z2m external converter (similar scope to the ZHA quirk, ~80 lines of JS) is on the backlog.

## Over-the-air firmware updates

The device advertises the ZCL OTA Upgrade cluster (0x0019) on endpoint 1 with dual-slot (`ota_0`/`ota_1`, 960 KB each) plus bootloader rollback. Updates ride Zigbee — no USB cable after the first flash.

### Identity

| Field | Value |
|---|---|
| Manufacturer code | `0x1289` |
| Image type | `0x0001` |
| Current file version | `0x00000002` |

Bump `LIGHTMETER_FW_VERSION` in `main/main.c` with every release.

### Release

```
idf.py build
scripts/make_ota.py build/lightmeter.bin lightmeter-v3.ota \
    --manufacturer 0x1289 --image-type 0x0001 --version 0x00000003
# drop the .ota into <HA config>/zigbee_ota/ and reload ZHA
```

`make_ota.py` wraps an ESP-IDF `.bin` in the ZCL OTA Upgrade File format (header + single Upgrade Image sub-element). Expect 15–30 min per update over Zigbee. If the new image fails to rejoin within the rollback window, the bootloader reverts on next reset.

## Calibration

Per-band responsivities in `main/main.c` (`responsivity_basic_f1_f8` and `responsivity_basic_nir`) are AS7341 datasheet-typical values normalized into the k0i05 driver's "basic counts" domain. Expect **factor-of-2 accuracy** until single-point-calibrated against a reference meter (Apogee MQ-500 / LI-COR LI-250 / similar). To calibrate: scale each coefficient by `(firmware_reading_umol / reference_umol)` for that band, bump the version, release an OTA.

The photopic lux channel uses CIE 1931 V(λ) weights at the AS7341 band centers with each band treated as a delta at its center wavelength. Good enough to cross-check the PPFD channels against an ordinary lux meter, not certification-grade photometry.

## Repo layout

```
main/
├── CMakeLists.txt
├── idf_component.yml    # k0i05/esp_as7341 + espressif/esp-zigbee-lib
└── main.c               # entire firmware, ~450 LOC
partitions.csv           # otadata + ota_0 + ota_1 + zb_storage + zb_fct
sdkconfig.defaults       # target, OTA, rollback, 4 MB flash
zha_quirk/
└── acamilo_lightmeter.py   # HA ZHA quirk — drop into HA's custom_zha_quirks/
scripts/
└── make_ota.py          # package .bin into ZCL OTA Upgrade File
CMakeLists.txt           # top-level IDF project
README.md                # this file
```

## License

MIT.
