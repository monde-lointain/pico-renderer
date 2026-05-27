---
name: fixed-point
description: Reference for the renderer's fixed-point math — Q-formats, the no-UMULL multiply discipline on Cortex-M0+, the SIO hardware divider, and the floats-only-in-setup rule.
---

# Fixed-Point Math (RP2040 / Cortex-M0+)

## Formats (frozen in rdr/types.h)
- `fx16_16` (Q16.16) — matrices / transform.
- `fx12_4` (Q12.4) — screen coords (subpixel edge eval).
- `fx_invw` — 1/w depth (w-buffer); perspective yields inv_w directly.
- Colors 8-bit/channel internally; pack to 565/4444 on write.

## Multiply discipline — NO 64-bit multiply on ARMv6-M
- M0+ has `MULS` (32×32→32) only; **no `UMULL`/`MLA`**. A 32.32 product needs synthesis from 16×16 partials.
- Keep operands ≤16-bit where possible so a single `MULS` suffices. Choose Q-formats/ranges so products fit 32 bits.
- Reserve synthesized 64-bit products (16×16 partials) for the few places that truly need them; the transform/edge-setup hot path is a hand-tuned-asm candidate.

## Divide
- Use the SIO hardware divider (~8 cyc, per-core) for the perspective divide (`w = 1/inv_w`). Don't open-code division.

## Floats
- Floats appear ONLY in setup and asset tools, NEVER in inner loops. Host golden tests use a float oracle to validate the fixed-point path within tolerance; host↔device fixed-point must be bit-identical.
