---
"@hevcjs/core": patch
---

Further speed up the single-threaded (browser/WASM) HEVC decode path, targeting
the post-SIMD hotspots — **+31% at 1080p** (38.9 → 50.9 fps single-thread) and
**+28% at 4K** (9.2 → 11.8 fps). No API or output change: all 103
conformance/unit tests pass, including the pixel-perfect oracle suite (8- and
10-bit), and the build is AddressSanitizer-clean.

Re-profiling after the previous SIMD pass showed Sample Adaptive Offset had
become the dominant cost at 4K (≈37% of instructions, ≈40% of data reads), so
this pass attacks it and the next tier:

- SSE2/wasm128 SAO edge-offset and band-offset inner loops (sign via packed
  compares, offset-table select via masked accumulate — kept to baseline SSE2,
  no SSSE3 `pshufb`, so emscripten lowers cleanly to wasm128).
- Hoisted SAO's per-pixel cross-slice/tile boundary test to a picture-level
  pre-check; it only affects multi-slice or tiled streams, and was previously
  evaluated for every sample (and was blocking the vectorised path entirely).
- Skip the SAO pre-filter backup copy for any colour component that has no SAO
  anywhere in the picture (chroma SAO is frequently disabled).
- SSE2/wasm128 default weighted sample prediction (uni- and bi-prediction
  averaging + clip), computed in int32 to stay bit-exact.

As before, the SSE2 intrinsics compile natively on x86-64 and lower to wasm128
under `-msse2`, so the native conformance oracle verifies the exact code shipped
to WebAssembly.
