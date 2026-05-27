---
name: golden-image-test
description: Use when validating geometry/raster/shade correctness on host — write a float-oracle reference, compare the fixed-point output against it (and against committed golden images) within tolerance.
---

# Golden-Image / Oracle Testing

## Two comparisons
1. **Correctness vs. a float oracle:** a simple, slow floating-point reference of the same transform/raster/shade math. Compare the fixed-point output to the oracle within a stated tolerance — catches systematic fixed-point/geometry bugs both targets would otherwise share.
2. **Host↔device parity:** fixed-point output must be **bit-identical** across host and device (no float in inner loops). 

## Procedure
1. Implement the oracle in `tests/harness/` (float, readable, not performance-bound).
2. Render the same input through the module under test (fixed-point).
3. Compare per-pixel/per-value within tolerance; on mismatch, dump both + the diff.
4. **Inject an error** (perturb one input) and confirm the test fails — prove the comparison is wired before trusting green.
5. For full-frame goldens: store/compare a reference image (storage decision — commit PNG vs generate-and-cache — is Unresolved Q5; pick per B.1-ε).
