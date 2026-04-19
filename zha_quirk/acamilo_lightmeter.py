"""ZHA quirk for the Acamilo lightmeter (AS7341 + ESP32-H2).

Why this file exists: ZHA's generic AnalogInput -> sensor auto-discovery
doesn't reliably surface multi-endpoint analog sensors as HA entities.
This quirk matches on Basic cluster manufacturer/model and explicitly
declares one sensor per endpoint so every channel shows up.

Install:
  1. Put this file in /config/custom_zha_quirks/ on the HA host.
  2. Add to configuration.yaml:
         zha:
           custom_quirks_path: /config/custom_zha_quirks
  3. Restart HA. Remove the existing lightmeter device, erase-flash the
     firmware so it forgets the old network, and re-pair.

Unit story: HA rejects arbitrary string units (ZHA's validate_unit only
accepts entries from its internal UNITS_OF_MEASURE registry). For the 9
PPFD channels + NIR the unit is µmol/m²/s, which isn't in HA's catalog,
so we leave `unit` unset and let the user customize it per entity in the
UI. The photopic-illuminance channel gets the real lux enum.
"""
from zigpy.quirks.v2 import QuirkBuilder, ReportingConfig
try:
    from zigpy.quirks.v2.homeassistant import UnitOfIlluminance
except ImportError:  # older/newer zigpy may name it differently
    try:
        from zigpy.quirks.v2.homeassistant import UnitOfLightIlluminance as UnitOfIlluminance
    except ImportError:
        UnitOfIlluminance = None  # fall back to no unit on the lux channel too
from zigpy.zcl.clusters.general import AnalogInput, BinaryInput

MANUFACTURER = "Espressif"
MODEL = "lightmeter"

# (endpoint_id, translation_key, fallback_name, unit-or-None)
ANALOG_CHANNELS = [
    ( 1, "f1_415_ppfd", "F1 415nm PPFD", None),
    ( 2, "f2_445_ppfd", "F2 445nm PPFD", None),
    ( 3, "f3_480_ppfd", "F3 480nm PPFD", None),
    ( 4, "f4_515_ppfd", "F4 515nm PPFD", None),
    ( 5, "f5_555_ppfd", "F5 555nm PPFD", None),
    ( 6, "f6_590_ppfd", "F6 590nm PPFD", None),
    ( 7, "f7_630_ppfd", "F7 630nm PPFD", None),
    ( 8, "f8_680_ppfd", "F8 680nm PPFD", None),
    ( 9, "par_total",   "PAR total",     None),
    (10, "illuminance", "Illuminance",   UnitOfIlluminance.LUX if UnitOfIlluminance else None),
    (11, "nir_pfd",     "NIR 910nm PFD", None),
]

BINARY_CHANNELS = [
    (12, "flicker_detected",    "Flicker (100/120 Hz) detected"),
    (13, "spectral_saturated",  "Spectral channel saturated"),
]

# Push ConfigureReporting on join so the coordinator gets pushed updates
# instead of leaving PresentValue at "unknown" until it happens to poll.
#   - analog channels: change-driven (delta=0.1) with a 60 s heartbeat
#   - binary flags:    any transition, with a 5 min heartbeat
ANALOG_REPORTING = ReportingConfig(
    min_interval=2, max_interval=60, reportable_change=0.1,
)
BINARY_REPORTING = ReportingConfig(
    min_interval=1, max_interval=300, reportable_change=1,
)

builder = QuirkBuilder(MANUFACTURER, MODEL)

for ep, tkey, name, unit in ANALOG_CHANNELS:
    kwargs = {
        "endpoint_id":      ep,
        "translation_key":  tkey,
        "fallback_name":    name,
        "reporting_config": ANALOG_REPORTING,
    }
    if unit is not None:
        kwargs["unit"] = unit
    builder = builder.sensor(
        AnalogInput.AttributeDefs.present_value.name,
        AnalogInput.cluster_id,
        **kwargs,
    )

for ep, tkey, name in BINARY_CHANNELS:
    builder = builder.binary_sensor(
        BinaryInput.AttributeDefs.present_value.name,
        BinaryInput.cluster_id,
        endpoint_id=ep,
        translation_key=tkey,
        fallback_name=name,
        reporting_config=BINARY_REPORTING,
    )

builder.add_to_registry()
