---
"@hevcjs/core": patch
---

Next-tier decode perf after re-profiling the post-SIMD hot path (callgrind):
vectorize the pixel-write cluster and table-drive the hottest CABAC context.

- **SIMD reconstruct_block / store_pred_block** — these scalar per-sample pixel
  writes were the last un-vectorized hot kernels (store_pred_block alone owned
  ~15% of all L1 data write-misses). They now write rows via SSE2/wasm128:
  saturating int16 add + `packus` (uint8, the [0,255] clip is free) or min/max
  clip (uint16). Bit-exact (saturating add then clip equals the scalar int add +
  Clip3 since maxVal < int16 max).
- **Table-driven sig_coeff_flag context** — `decode_residual_coding` is the #1
  branch-mispredict source; its per-coefficient context derivation ran a
  `switch(prevCsbf)` + nested ternaries up to ~1024× per 32×32 TU. Replaced with
  a precomputed `sigCtxBase[prevCsbf][(yP<<2)+xP]` lookup (spec eqs 9-45…9-48).

WASM single-thread decode vs the prior build: **+4% at 1080p, +3% at 4K**.
Bit-exact — all 106 conformance/oracle tests pass (8- and 10-bit),
AddressSanitizer-clean.
