---
name: on-target-probe
description: Use to run a test/measurement on the PicoSystem hardware — build the .uf2, flash via picotool, read KEY=VALUE lines over USB-CDC serial, parse and assert thresholds.
---

# On-Target Probe

## Procedure
1. Build the `.uf2` (pico preset). Firmware MUST link picotool's reset/USB-stdio interface and arm a hardware watchdog (so a hung frame auto-reboots and picotool can reset it).
2. Flash: `picotool reboot -f -u` (reboot running pico into BOOTSEL) then `picotool load -x build-pico/.../<fw>.uf2`. **Fallback:** if the device is hung and won't reset, emit "press BOOTSEL" and wait — human in the loop.
3. Wait for USB re-enumeration: poll for `/dev/ttyACM*` with a timeout.
4. Read serial lines until a terminating sentinel (e.g. `PROBE_DONE`); each is `KEY=VALUE`.
5. Parse and assert thresholds; on parse failure, re-run and dump raw lines.
6. Visual checks (AA/tearing/banding) are HUMAN — emit the observation protocol and the expected result.

## Note
Only one device — on-target runs are serialized (barrier/perf-gate, not per-stream).
