---
"@hevcjs/core": patch
---

SIMD the horizontal-edge luma deblocking filter for a further single-threaded
(browser/WASM) decode speedup — **~1.4% at 1080p** and **~0.9% at 4K**, on top
of the previous SIMD passes. No API or output change: all 103 conformance/unit
tests pass, including the pixel-perfect oracle suite (8- and 10-bit), and the
shipped WASM output is byte-identical (verified by per-frame SHA-256 over all
cropped planes).

Line-level profiling of `apply_deblocking` (callgrind, native serial proxy)
showed the filter *arithmetic* is only ~7% of deblocking cost — the bulk is
branchy boundary-strength derivation and scattered metadata reads, which are not
vectorizable. The one structurally addressable hotspot is the horizontal-edge
sample I/O: there the four filtered lines are four contiguous columns, so each
perpendicular sample position is a 4-pixel run. The new SSE2/wasm128 path
(`deblock_luma_hor_simd`) loads/filters/stores all four lines at once
(strong + weak, with per-line masking and PCM / transquant-bypass suppression),
replacing the per-line scalar loop that strided one address-multiply per sample.
Vertical edges keep the (already contiguous, cache-friendly) scalar path.
