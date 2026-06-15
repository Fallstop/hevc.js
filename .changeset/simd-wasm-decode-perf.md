---
"@hevcjs/core": patch
---

Speed up the single-threaded (browser/WASM) HEVC decode path with portable,
bit-exact SIMD and reduced memory traffic — **+24.5% at 1080p** (30.9 → 38.5 fps
single-thread; native serial 1.21×). No API or output change: all 103
conformance/unit tests pass, including the pixel-perfect oracle suite (8- and
10-bit), and the build is AddressSanitizer-clean.

- SSE2 motion-compensation interpolation (luma 8-tap / chroma 4-tap) via an
  `_mm_madd_epi16` kernel that lowers to a single wasm `i32x4.dot_i16x8_s`.
- Column-parallel SSE2 `idct16` / `idct32` inverse transforms.
- Dropped redundant per-transform-unit coefficient/residual zero-initialisation
  (the largest source of L1 write-misses).
- Branchless SAO offset application (removes the decoder's worst-predicted branch).

The SSE2 intrinsics compile natively on x86-64 and lower to wasm128 under
`-msse2`, so the native conformance oracle verifies the exact code that ships
to WebAssembly.
