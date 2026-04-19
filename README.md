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

## Home Assistant integration

Two pieces ship in this repo alongside the firmware:

| Path | Purpose |
|---|---|
| `zha_quirk/acamilo_lightmeter.py` | zigpy v2 quirk — forces ZHA to create one sensor / binary_sensor entity per endpoint, which its generic AnalogInput auto-discovery won't do reliably for a 13-endpoint device. |
| `ha_card/lightmeter-card.js` | Vanilla Lovelace custom card — renders the 9-band spectrum as coloured bars with PAR / lux headline numbers and flicker / saturation chips. Zero dependencies. |

Both are optional — the device works as a plain ZHA sensor without them — but you want both for the full UX.

### Prerequisites

- HA 2024.4+ for the v2 QuirkBuilder API (older versions need the v1 `CustomDevice` pattern — ask).
- A working ZHA coordinator already paired with HA.
- Filesystem access to `/config/` on the HA host. The **Official "Terminal & SSH" add-on** works for this; so does the **File editor** add-on if you'd rather do it in the browser.

### 1. Drop the files

From a checkout of this repo, with SSH to your HA host (replace the address and user):

```sh
# quirk — goes next to HA's config so zigpy discovers it
ssh user@ha.local 'sudo mkdir -p /config/custom_zha_quirks'
scp zha_quirk/acamilo_lightmeter.py user@ha.local:/tmp/
ssh user@ha.local 'sudo mv /tmp/acamilo_lightmeter.py /config/custom_zha_quirks/'

# card — served out of HA's www/ as /local/…
scp ha_card/lightmeter-card.js user@ha.local:/tmp/
ssh user@ha.local 'sudo mkdir -p /config/www && sudo mv /tmp/lightmeter-card.js /config/www/'
```

If you're using the **File editor** add-on instead: paste the contents of each file into `custom_zha_quirks/acamilo_lightmeter.py` and `www/lightmeter-card.js` via the web editor, creating the directories as needed.

### 2. Point ZHA at the custom quirks path

Add this to `configuration.yaml` (if the `zha:` block already exists, just add the key under it):

```yaml
zha:
  custom_quirks_path: /config/custom_zha_quirks
```

### 3. Register the card as a Lovelace resource

**Settings → Dashboards → ⋮ → Resources → Add resource**

- URL: `/local/lightmeter-card.js`
- Resource type: **JavaScript module**

(Storage-mode dashboards only. YAML-mode dashboards: add the same URL under `lovelace.resources` in `configuration.yaml` instead.)

### 4. Restart HA Core

**Settings → System → Restart Home Assistant → Restart Home Assistant Core**, or from a shell with supervisor access: `ha core restart`.

### 5. Pair the device

1. **Settings → Devices & Services → ZHA → Add device.** Permit-join opens for 60 s.
2. Power-cycle the lightmeter. It boots factory-new, finds the network, and joins automatically — no install code.
3. The device appears as **Espressif lightmeter** with 11 `sensor.*` entities and 2 `binary_sensor.*` (diagnostic) entities — one per endpoint. The quirk sets friendly names per band.

If you previously paired the device **without** the quirk installed, remove the device first (its cached cluster metadata won't pick up the quirk otherwise), then re-pair.

### 6. Add the card to a dashboard

Dashboard → Edit → **+ Add card** → scroll to **Manual** → paste:

```yaml
type: custom:lightmeter-card
```

Optional config keys:

```yaml
type: custom:lightmeter-card
title: Grow Room Spectrum
entity_base: sensor.espressif_lightmeter
binary_base: binary_sensor.espressif_lightmeter
```

### Unit labels

ZHA's unit system rejects arbitrary strings, so the µmol/m²/s channels appear without a default unit (only the lux channel gets `lx` automatically). Two fixes:

- **Per entity**, in the HA UI: click the entity → ⚙ Settings → set **Unit of measurement** to `µmol/m²/s`. Do this once per PPFD entity.
- **Bulk**, via the REST API (requires a long-lived access token):
  ```sh
  TOKEN=...
  for ent in f1_415nm_ppfd f2_445nm_ppfd f3_480nm_ppfd f4_515nm_ppfd \
             f5_555nm_ppfd f6_590nm_ppfd f7_630nm_ppfd f8_680nm_ppfd \
             par_total nir_910nm_pfd; do
      curl -X POST -H "Authorization: Bearer $TOKEN" \
           -H "Content-Type: application/json" \
           -d '{"unit_of_measurement":"µmol/m²/s"}' \
           "http://<ha>:8123/api/config/entity_registry/sensor.espressif_lightmeter_$ent"
  done
  ```

### Updating either file

After editing the quirk, restart HA so zigpy re-imports it. After editing the card, either hard-reload your browser (Ctrl+Shift+R) or bump the resource URL's cache-bust (`/local/lightmeter-card.js?v=N`) in the Lovelace resource settings.

## Zigbee2MQTT

A standards-compliant device — z2m picks it up, but its default `numeric` converter gives generic names. For pretty entities, a z2m external converter (similar scope to the ZHA quirk, ~80 lines of JS) is on the backlog.

## Over-the-air firmware updates

The device advertises the ZCL OTA Upgrade cluster (0x0019) on endpoint 1 with dual-slot (`ota_0`/`ota_1`, 960 KB each) plus bootloader rollback. Updates ride Zigbee — no USB cable after the first flash.

### Identity

| Field | Value |
|---|---|
| Manufacturer code | `0x1289` |
| Image type | `0x0001` |
| Current file version | `0x00000007` |

Bump `LIGHTMETER_FW_VERSION` in `main/main.c` with every release.

### Release

```
idf.py build
scripts/make_ota.py build/lightmeter.bin lightmeter-v8.ota \
    --manufacturer 0x1289 --image-type 0x0001 --version 0x00000008
```

`make_ota.py` wraps an ESP-IDF `.bin` in the ZCL OTA Upgrade File format (header + single Upgrade Image sub-element). Expect 15–30 min per update over Zigbee. If the new image fails to rejoin within the rollback window, the bootloader reverts on next reset.

### Configuring ZHA to serve the image (one-time)

ZHA needs an OTA provider pointed at your image directory. The right type is **`advanced`** (maps to zigpy's `AdvancedFileProvider` — recursively scans a directory for `.ota` files).

Two gotchas cost me an hour figuring this out:

1. **Not `zigpy_local`** — that type expects a JSON index file, not a directory of `.ota` binaries. Using it with a directory path silently does nothing.
2. **`warning` field is required** — a verbatim safety-acknowledgment string. Omit it or mistype it and the provider silently fails to load. No log message.

Via the ZHA integration UI: Settings → Devices & Services → ZHA → **Configure** → OTA providers → **+ Add** → pick `advanced`, path `/config/zigpy_ota`, tick the "I understand…" box.

Or, via `.storage/core.config_entries` directly, merge this into the ZHA entry's `options`:

```json
"ota_providers": [{
  "type": "advanced",
  "path": "/config/zigpy_ota",
  "warning": "I understand I can *destroy* my devices by enabling OTA updates from files. Some OTA updates can be mistakenly applied to the wrong device, breaking it. I am consciously using this at my own risk."
}]
```

Restart HA. ZHA now scans `/config/zigpy_ota/` for `.ota` files on startup. Drop `lightmeter-v*.ota` into that directory — ZHA matches on the embedded manufacturer + image type, compares file_version, and offers the update via the `update.espressif_lightmeter_firmware` entity. Click **Install** on that entity (or call `update.install`) to trigger the transfer.

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
