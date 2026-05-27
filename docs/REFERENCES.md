# Primary-Source References (MANDATORY for all implementation subagents)

**Rule:** Before writing any code, **read the references relevant to your module** (below). **Never
hallucinate a software API, register, hardware behavior, format, or timing** — verify it against the
SDK headers, datasheets, manuals, and emulators listed here. If a reference and your assumption
disagree, the reference wins. These are paths to **read selectively** (grep the headers, read the
relevant datasheet pages / manual sections) — do not ingest entire repos.

## References
- **Raspberry Pi Pico SDK:** `~/development/repos/pico-sdk` (APIs: multicore, DMA, PIO, SIO divider,
  `hardware_*`, `pico_stdio_usb`, watchdog, interpolators)
- **PicoSystem SDK:** `~/development/repos/picosystem` (reference ONLY for how to drive the
  peripherals — display/ST7789, audio, buttons; we write our own atop pico-sdk)
- **N64 Programming Manual:** `~/development/repos/n64sdkmod/packages/n64manual/usr/share/doc/n64sdk/pro-man`
  (RSP geometry/transform, RDP rasterizer, color combiner, blender, TMEM, texture formats, AA/coverage)
- **RDP/VI Software Emulator:** `~/development/repos/angrylion-rdp-plus` (exact rasterizer / blender /
  coverage / VI-filter behavior — GPL; adaptable per our GPL license)
- **RSP Software Emulator:** `~/development/repos/parallel-n64` (RSP microcode / vector-unit behavior)
- **N64 GBI:** `~/development/repos/libultra_modern/include/PR/gbi.h` (command/display-list structs,
  `Vtx` union, matrix ops, format enums — our command model is GBI-inspired)
- **ST7789 datasheet:** `~/Documents/datasheets/ST7789.pdf` · **Adafruit ST7789 driver:**
  `~/development/repos/Adafruit-ST7735-Library` (COLMOD/MADCTL, init sequence, RGB565 path)
- **RP2040 datasheet (summaries):** `~/Documents/datasheets/summaries` · **PicoSystem schematic:**
  `~/Documents/datasheets/picosystem_schematic.pdf` (no-UMULL/ARMv6-M, SIO divider, DMA, PIO, pinout,
  display/audio wiring)

## Per-module relevance guide (read at least these before implementing)
| Stream / module | Read first |
|---|---|
| `fixed` | RP2040 datasheet (ARMv6-M: no UMULL/MLA, MULS only; SIO HW divider; interpolators) |
| `geom`, `clip` | N64 manual (RSP transform/clip/lighting); `gbi.h` (`Vtx`, matrices); RP2040 (math) |
| `raster`, `aa` | angrylion-rdp-plus (edge/coverage/fill); N64 manual (RDP rasterizer, AA coverage) |
| `tex` | N64 manual (TMEM, texture formats CI/IA, filtering); `gbi.h` (format enums) |
| `shade`, `blend` | N64 manual (color combiner `(A−B)·C+D`, blender modes, dither); angrylion (exact ops) |
| `cmd`, `arena` | `gbi.h` (display-list/command structure, branch/call); pico-sdk (memory) |
| `sched` | pico-sdk (multicore FIFO, spinlocks, DMA, `__not_in_flash_func`); RP2040 datasheet |
| `platform` | ST7789 datasheet + Adafruit driver (COLMOD 0x55/565, init); pico-sdk PIO/DMA; picosystem SDK (screen.pio reference); schematic (pins) |
| `on-target-runner` | pico-sdk (`pico_stdio_usb`, watchdog, picotool reset interface); RP2040 datasheet |
| assets (`tools`) | `gbi.h` + N64 manual (texture/vertex formats, swizzle, TLUT) |
