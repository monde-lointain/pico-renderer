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

## Firmware checklist (hard requirements — learned the hard way, see WORKFLOW.md)
Before flashing ANY firmware (spikes included):
- **RAM via linker symbols** (`&__HeapLimit - &__end__`), **never malloc-until-fail** — pico
  `malloc` *panics* on OOM, and a panic wedges USB so `picotool` can't software-reset it.
- **Arm a hardware watchdog + link the picotool reset stub** so a hang/panic auto-recovers
  instead of forcing a manual BOOTSEL.
- **Benchmarks must defeat dead-code elimination** (volatile sinks / asm barriers) and **set the
  250 MHz target clock** before timing — else numbers are bogus/unrepresentative.

## Probe-validity checklist (TW-04 — a wrong probe gives a confidently-wrong number)
A measurement is only trustworthy if the probe itself is valid. Confirm ALL before believing a number:
- **Build:** Release / `-O3` / `NDEBUG` (a Debug build measures the wrong code).
- **Hot code in SRAM:** the timed path is `__not_in_flash_func` (or otherwise SRAM-resident) when
  the question is compute, not XIP — otherwise you measure flash-fetch stalls by accident.
- **Anti-DCE:** the result feeds a `volatile` sink / asm barrier so the compiler can't elide the work.
- **Clock confirmed:** read back `SYSCLK` (don't assume the overclock took).
- **HW registers via the SDK struct, never hardcoded offsets** — the S0 spike's prompt had
  `CTR_ACC=0x08`; the real field is `0x10` (use the SDK's register struct so the layout is correct).
- **Right timer for the span (T5):** `time_us_64` (1µs, 64-bit, no-wrap, cross-core) for any span
  that can approach/exceed SysTick's ~67ms wrap (24-bit @250MHz); SysTick/FINE ONLY for spans
  *guaranteed* <67ms where 1µs is too coarse. **Reset CVR at each fine-block start** so COUNTFLAG
  is a true ">67ms" tripwire (free-running, it false-positives on any block that merely straddles
  zero). A `wrap=1` on a fine anchor MEANS "move that anchor to the coarse (`time_us_64`) timer".
- **Flash + capture back-to-back, plain `cat` to read (T5):** a BUSY device (both cores 100%) makes
  `picotool` reboot-to-BOOTSEL slow/wedge — flash an IDLE device, and for a bounded run
  (e.g. `DEMO_FRAMES=480` ≈ 290s) attach the reader WITHIN the run window or you capture 0 lines.
  A plain `cat /dev/ttyACM*` (no DTR toggle) reads the CDC tty reliably; a pyserial open that
  asserts DTR disturbs the CDC and races re-enumeration → 0 lines. Budget a physical BOOTSEL /
  power-cycle: repeated forced reboots wedge USB.

## Prerequisite: CDC serial access (set up once at S0)
`/dev/ttyACM*` is `root:dialout`; `usermod -aG dialout` does NOT bind mid-session. Install a tty
udev rule so it is group `plugdev` (no re-login):
`SUBSYSTEM=="tty", SUBSYSTEMS=="usb", ATTRS{idVendor}=="2e8a", GROUP="plugdev", MODE="0660", TAG+="uaccess"`
Throwaway spike firmware lives in its **own worktree** (see `worktree-pr`), never branch-switched.
