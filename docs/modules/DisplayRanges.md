# Display ranges

Display ranges are presentation metadata on `ValueRegistry` slots. They do not
change a sample, its quality, history, alarm evaluation, or actuator limits.
`hasDisplayRange` distinguishes an absent range from a numeric range. Numeric
bounds must be finite and satisfy `displayMin < displayMax`; Boolean values do
not accept a range.

## Defaults and storage

The registry owns the current default scales for stable `ValueId` values 0
(ORP), 1 (pH), 4 (water temperature), and 5 (air temperature). Other values
default to no display range. The historical color bands in
`src/Modules/IOModule/text/runtimeui.json` remain independent threshold bands;
their scale endpoints are omitted and the effective range comes from the
registry.

Overrides use fixed NVS pages `dvr00` onward. Each version 1 page covers 64
consecutive stable IDs and stores a per-ID state: default, custom, or explicitly
no range. A missing page, unknown version, malformed page, or invalid record
falls back to the registry default. There is no migration from earlier display
range settings because none were persisted before this format. Exposure choices
keep their existing indexed ConfigStore entries; display pages are separate
from those choices and remain available when a measurement is disabled and
re-enabled.

The full range state array is allocated with `DataExposureConfig` in PSRAM: for
the current `ValueIds::Capacity` of 816 and a 24-byte aligned record, that is
19,584 bytes. On the current target ABI, `ValueMetadata` grows from 112 to 128
bytes, adding 16 bytes to each allocated registry slot (up to 13,056 bytes at
full capacity), also in PSRAM. NVS stores one 1,090-byte page only after a range
on that page is changed; the worst case is 13 pages (14,170 bytes of blob
payload). No writes occur during acquisition. A failed page write leaves the
runtime range unchanged and the Web interface reports the failure.

## Interfaces

The Web `Diffusion des données` page edits numeric ranges in a per-measure
dialog. `/api/data/values` exposes effective and default display bounds only to
the local Web interface; `/api/data/display` validates and stores changes. The
runtime Web values response also carries optional range metadata. Dashboard
sonde scales, runtime threshold views, and history chart axes use the effective
range. History records and raw displayed numbers are not clamped; chart
coordinates are clipped to the configured axis.

Home Assistant Discovery and MQTT value payloads do not contain display bounds.
Discovery continues to use the metadata unit. Command `number` min/max remain
the separate actuator constraints.

TFT S3 and Nextion screens do not currently use a measurement range to scale a
numeric display; TFT measurement fields are numeric text, with no scale to
update. The Nextion home page's pH/ORP needle percentage is calculated
from control setpoints by the ESP32 and is not a measurement display scale;
changing it to use display bounds would alter that gauge's meaning and requires
a separate Nextion protocol/firmware design. Existing Nextion TFT artifacts
were not modified. Numeric-only TFT/Nextion labels continue to show the measured
value without a gauge.
