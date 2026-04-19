"""ZHA quirk for the Acamilo lightmeter (AS7341 + ESP32-H2).

Why this file exists: Home Assistant's ZHA integration will discover the
Basic/Identify/Analog-Input/Binary-Input clusters our firmware advertises,
but its generic AnalogInput -> sensor auto-discovery is flaky for devices
with many endpoints. This quirk matches on the Basic cluster's
manufacturer/model strings (set in main/main.c -> BASIC_MANUFACTURER_NAME
/ BASIC_MODEL_IDENTIFIER) and explicitly declares a sensor entity per
endpoint with the right unit and human-readable name.

Install:
  1. Put this file in <HA config>/custom_zha_quirks/  (create that
     directory if it does not exist, then set
     `zha.configuration.custom_quirks_path: custom_zha_quirks` in
     configuration.yaml and restart HA; or use the ZHA integration UI's
     "Custom quirks path" option in modern HA releases).
  2. Remove the existing lightmeter device from ZHA.
  3. Erase-flash the firmware (so it forgets the previous network) and
     re-pair.

Uses the v2 QuirkBuilder API (zigpy >= 0.60 / HA >= 2024.x). If your
HA/zigpy is older, let me know and I will port to the v1 CustomDevice
style.
"""
from zigpy.quirks.v2 import QuirkBuilder
from zigpy.zcl.clusters.general import AnalogInput, BinaryInput

MANUFACTURER = "Espressif"
MODEL = "lightmeter"

# (endpoint_id, translation_key, fallback_name, unit)
ANALOG_CHANNELS = [
    ( 1, "f1_415_ppfd", "F1 415nm PPFD", "µmol/m²/s"),
    ( 2, "f2_445_ppfd", "F2 445nm PPFD", "µmol/m²/s"),
    ( 3, "f3_480_ppfd", "F3 480nm PPFD", "µmol/m²/s"),
    ( 4, "f4_515_ppfd", "F4 515nm PPFD", "µmol/m²/s"),
    ( 5, "f5_555_ppfd", "F5 555nm PPFD", "µmol/m²/s"),
    ( 6, "f6_590_ppfd", "F6 590nm PPFD", "µmol/m²/s"),
    ( 7, "f7_630_ppfd", "F7 630nm PPFD", "µmol/m²/s"),
    ( 8, "f8_680_ppfd", "F8 680nm PPFD", "µmol/m²/s"),
    ( 9, "par_total",   "PAR total",     "µmol/m²/s"),
    (10, "illuminance", "Illuminance",   "lx"),
    (11, "nir_ppfd",    "NIR 910nm PFD", "µmol/m²/s"),
]

# (endpoint_id, translation_key, fallback_name)
BINARY_CHANNELS = [
    (12, "flicker_detected", "Flicker (100/120 Hz) detected"),
    (13, "spectral_saturated", "Spectral channel saturated"),
]

builder = QuirkBuilder(MANUFACTURER, MODEL)

for ep, tkey, name, unit in ANALOG_CHANNELS:
    builder = builder.sensor(
        AnalogInput.AttributeDefs.present_value.name,
        AnalogInput.cluster_id,
        endpoint_id=ep,
        unit=unit,
        translation_key=tkey,
        fallback_name=name,
    )

for ep, tkey, name in BINARY_CHANNELS:
    builder = builder.binary_sensor(
        BinaryInput.AttributeDefs.present_value.name,
        BinaryInput.cluster_id,
        endpoint_id=ep,
        translation_key=tkey,
        fallback_name=name,
    )

builder.add_to_registry()
