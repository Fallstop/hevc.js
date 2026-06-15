---
"@hevcjs/core": patch
---

Two further bit-exact single-thread (browser/WASM) decode speedups, on top of
the prior SIMD passes. No API or output change: all conformance/oracle tests
pass (8- and 10-bit, pixel-perfect).

- **Deblocking boundary-strength strength-reduction.** In `derive_bs` the
  per-edge filter-grid and motion-info indexing used integer division/modulo by
  constants that are always powers of two on a non-negative operand (the
  4-sample filter grid, `MinTbSizeY`, and the per-edge TU size). These are now
  shifts and masks (`>>2`, `>>MinTbLog2SizeY`, `& (tuSize-1)`), bit-identical to
  the divisions but cheaper on WebAssembly where `i32` div/rem is not a single
  op.
- **Skip the SAO pre-filter backup copy for band-offset-only components.** SAO
  backed up each active component's whole plane before filtering, but band
  offset reads only the sample it writes — only edge offset needs the neighbour
  backup. The picture-level scan now also tracks whether any CTU uses edge
  offset per component; a component that is SAO-active but band-offset-only
  everywhere is filtered in place, removing a full-plane copy per such component
  per frame (a large avoidable streaming write/read at 4K, where chroma SAO is
  frequently band-only).
