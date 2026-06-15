---
"@hevcjs/core": patch
---

Finish vectorising the inverse-transform path and skip transform work outside
the non-zero coefficient region — both bit-exact. No API or output change; all
conformance/oracle tests pass (8- and 10-bit).

- **SSE2 `idct4` / `idct8` / `idst4` (DST-VII).** The small inverse transforms
  were the last scalar ones after `idct16`/`idct32` were vectorised; they now
  use the same column-parallel `_mm_madd_epi16` + `_mm_packs_epi32` kernel
  (saturation == `Clip3(-32768,32767)`, accumulators fit `int32`). 4x4/8x8 TUs
  dominate inter-predicted content, so these run very frequently.
- **Non-zero-region skip in `inverse_transform_2d` (§8.6.4.2).** The dequant
  pass now records the inclusive bounding box of non-zero coefficients; the
  two-pass transform skips columns/rows beyond it (an all-zero input column
  yields an all-zero output column, so those are memset rather than transformed)
  with an all-DC fast path. Bit-exact (zero coefficients contribute nothing to
  the linear transform); the win is largest on low-detail / inter content.
