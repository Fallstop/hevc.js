---
"@hevcjs/core": patch
---

Performance: faster decode hot paths. SAO now uses tight per-CTU fast paths
(branchless edge-offset, band-offset LUT) instead of per-pixel boundary
checks; luma/chroma interpolation uses row-pointer loops the compiler can
vectorize; the DPB recycles picture buffers instead of reallocating and
zero-filling multiple MB per frame; the release WASM build no longer ships
with Emscripten runtime assertions enabled; frame extraction and 8-bit I420
conversion in JS use bulk typed-array copies.
