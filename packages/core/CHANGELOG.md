# @hevcjs/core

## 1.4.0

### Minor Changes

- [`a2020e1`](https://github.com/privaloops/hevc.js/commit/a2020e15028b0eab9639a035c671eae0e2006317) Thanks [@Fallstop](https://github.com/Fallstop)! - Add a cheap decoder `reset()` and a zero-copy frame-output path.

  - **`HEVCDecoder.reset(clearParameterSets = true)`** — reset the decoder to its
    initial state and decode a new, independent stream on the _same_ WASM instance,
    without `destroy()` + `create()`. It drops the DPB and POC state (and, by
    default, the stored VPS/SPS/PPS) while retaining the instance, its thread pool,
    and its per-picture scratch allocations — so seeks and scrubs reuse one warm
    decoder instead of paying a full teardown/rebuild each time. Pass
    `clearParameterSets: false` to keep parameter sets when seeking within a stream
    that sent them once, out-of-band. Backed by a new `hevc_decoder_reset` C export.

  - **`HEVCDecoder.drainViews()`** — a zero-copy variant of `drain()` that returns
    frames whose planes are strided sub-array views directly into the WASM heap
    (carrying `strideY` / `strideC`), eliminating the per-frame plane allocation and
    copy. Views are valid only until the next decoder call; intended for same-thread
    consumers (e.g. the transcode worker). The H.264 encoder now narrows straight
    from these strided heap views into a reused I420 buffer, and the segment
    transcoder uses this path — removing the steady-state per-frame plane copy and
    the per-frame output allocation.

  No decode-output change: the native conformance/oracle suite (8- and 10-bit) and
  the WASM output hash are unchanged; decode-after-`reset()` is byte-identical to a
  fresh decode.

- [`8964893`](https://github.com/privaloops/hevc.js/commit/896489339a992a56d0b9443c38fade364d3902d6) Thanks [@Fallstop](https://github.com/Fallstop)! - Native 8-bit (uint8) pixel path — store and process 8-bit HEVC at one byte per
  sample instead of uint16, halving picture memory and the memory traffic through
  motion compensation, SAO, and deblocking. 10-bit content stays uint16.

  - The pixel-touching kernels (reconstruct, intra, motion-comp interpolation,
    deblocking, SAO) are templated on the plane storage width and dispatch once per
    picture on `bytes_per_sample`; all hot kernels gained native uint8 SIMD:
    motion-comp and deblocking load bytes and widen to int (bit-exact, half the
    load bytes), and SAO runs a true 16-lane `epu8` edge/band kernel (2× the
    throughput of the uint16 path). **WASM single-thread decode: +15% at 4K, +4% at
    1080p** vs the previous uint16+SIMD path; bit-exact (all 106 conformance/oracle
    tests pass, 8- and 10-bit, AddressSanitizer-clean).
  - Decoded frames now expose `bytesPerSample` (1 for native 8-bit, 2 for uint16),
    and the `HEVCFrame` / `HEVCFrameView` plane fields are `Uint8Array | Uint16Array`
    accordingly — so `drainViews()` hands 8-bit content native `Uint8Array` heap
    views, letting a consumer upload straight to a texture with no `>> 2` / uint16→
    uint8 conversion. The C `HEVCFrame` ABI gains a trailing `bytes_per_sample`
    field (appended; existing field offsets unchanged).

### Patch Changes

- [`081e846`](https://github.com/privaloops/hevc.js/commit/081e846d299a8458f2fa2d9b7cd2329f6b279594) Thanks [@Fallstop](https://github.com/Fallstop)! - Cut the per-edge metadata overhead in the HEVC deblocking filter — the dominant
  cost at 4K, well above the filter arithmetic itself. Two picture-constant
  quantities are now derived once per frame instead of per 4-sample edge segment:

  - **Reference-picture POCs.** The boundary-strength derivation (§8.7.2.4.5)
    compares pictures by POC; it previously chased `dpb->ref_pic_listX(idx)->poc`
    (vector + `Picture*` indirections) for every inter edge. POCs are constant for
    the whole deblocking pass, so they are precomputed into small flat arrays and
    indexed by ref index.
  - **Tile / slice boundary exclusion.** For the common case (no tiles, a single
    slice, deblocking enabled) only the picture boundary can exclude an edge, so
    `is_boundary_excluded` takes a fast path that skips the per-segment CTB-address,
    slice-header, tile and slice-boundary work. The full path is unchanged and is
    selected whenever the picture actually uses tiles, multiple slices, or
    per-slice deblocking/across-slice flags.

  Together these drop `apply_deblocking` instructions ~15% and its branch
  mispredictions ~31% at 4K (native callgrind). Bit-exact: all 103 conformance/
  unit tests pass (incl. 8- and 10-bit oracle) and the shipped WASM output SHA-256
  is byte-identical.

- [`98505c5`](https://github.com/privaloops/hevc.js/commit/98505c52eda5f705870115bbd7dc5a983ee836dc) Thanks [@Fallstop](https://github.com/Fallstop)! - Next-tier decode perf after re-profiling the post-SIMD hot path (callgrind):
  vectorize the pixel-write cluster and table-drive the hottest CABAC context.

  - **SIMD reconstruct_block / store_pred_block** — these scalar per-sample pixel
    writes were the last un-vectorized hot kernels (store_pred_block alone owned
    ~15% of all L1 data write-misses). They now write rows via SSE2/wasm128:
    saturating int16 add + `packus` (uint8, the [0,255] clip is free) or min/max
    clip (uint16). Bit-exact (saturating add then clip equals the scalar int add +
    Clip3 since maxVal < int16 max).
  - **Table-driven sig_coeff_flag context** — `decode_residual_coding` is the [#1](https://github.com/privaloops/hevc.js/issues/1)
    branch-mispredict source; its per-coefficient context derivation ran a
    `switch(prevCsbf)` + nested ternaries up to ~1024× per 32×32 TU. Replaced with
    a precomputed `sigCtxBase[prevCsbf][(yP<<2)+xP]` lookup (spec eqs 9-45…9-48).

  WASM single-thread decode vs the prior build: **+4% at 1080p, +3% at 4K**.
  Bit-exact — all 106 conformance/oracle tests pass (8- and 10-bit),
  AddressSanitizer-clean.

- [`bc90311`](https://github.com/privaloops/hevc.js/commit/bc90311db2ae0d54a584e8a64f2fdf1afcd96e7b) Thanks [@Fallstop](https://github.com/Fallstop)! - SIMD the horizontal-edge luma deblocking filter for a further single-threaded
  (browser/WASM) decode speedup — **~1.4% at 1080p** and **~0.9% at 4K**, on top
  of the previous SIMD passes. No API or output change: all 103 conformance/unit
  tests pass, including the pixel-perfect oracle suite (8- and 10-bit), and the
  shipped WASM output is byte-identical (verified by per-frame SHA-256 over all
  cropped planes).

  Line-level profiling of `apply_deblocking` (callgrind, native serial proxy)
  showed the filter _arithmetic_ is only ~7% of deblocking cost — the bulk is
  branchy boundary-strength derivation and scattered metadata reads, which are not
  vectorizable. The one structurally addressable hotspot is the horizontal-edge
  sample I/O: there the four filtered lines are four contiguous columns, so each
  perpendicular sample position is a 4-pixel run. The new SSE2/wasm128 path
  (`deblock_luma_hor_simd`) loads/filters/stores all four lines at once
  (strong + weak, with per-line masking and PCM / transquant-bypass suppression),
  replacing the per-line scalar loop that strided one address-multiply per sample.
  Vertical edges keep the (already contiguous, cache-friendly) scalar path.

- [`953bb21`](https://github.com/privaloops/hevc.js/commit/953bb2127bf2fdc1a4f6d9d7174ae20c8c2e2b45) Thanks [@Fallstop](https://github.com/Fallstop)! - Further speed up the single-threaded (browser/WASM) HEVC decode path, targeting
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

- [`cc97762`](https://github.com/privaloops/hevc.js/commit/cc9776240e379e9046d7ec194b6a4800f97c137d) Thanks [@Fallstop](https://github.com/Fallstop)! - Speed up the single-threaded (browser/WASM) HEVC decode path with portable,
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

## 1.3.0

### Minor Changes

- [#137](https://github.com/privaloops/hevc.js/pull/137) [`6c2bf32`](https://github.com/privaloops/hevc.js/commit/6c2bf32c7ae704a9f341d95a407bb58313dee955) Thanks [@privaloops](https://github.com/privaloops)! - Add compute-aware ABR feedback for the Shaka and dash.js plugins. **On by default.**

  Mainstream player ABR algorithms pick a variant from network bandwidth
  alone — in a normal pipeline, fetch + parse + MSE append is essentially
  free compared to the network. With these plugins we add a real
  client-side cost: WASM HEVC decode + WebCodecs H.264 encode. A variant
  can be perfectly reachable from a bandwidth standpoint and still saturate
  the device's transcode budget, draining the buffer without the ABR
  algorithm ever noticing.

  This release adds a player-agnostic decider in `@hevcjs/core`
  (`ComputeAwareDecider`) plus a perf bus (`subscribeSegmentStat`) the
  transcoder publishes to after each segment. Both plugins ship an adapter
  that subscribes to the bus and narrows the variants the host ABR is
  allowed to choose from — via Shaka's
  `player.configure({ abr: { restrictions } })` and dash.js's
  `player.updateSettings({ streaming: { abr: { maxBitrate } } })`. The
  host ABR controller is never replaced.

  Usage (Shaka — needs an extra `attachComputeAware(player)` because the
  player doesn't exist at registration time):

  ```js
  // On by default.
  const handle = registerHevcTransmuxer(shaka, { wasmUrl, workerUrl });
  const player = new shaka.Player();
  handle.attachComputeAware(player);
  // To tune: { adaptiveCompute: { targetSpeedX: 1.5, lowerAfter: 1 } }
  // To opt out: { adaptiveCompute: false }
  ```

  Usage (dash.js — wires directly, the player is already available):

  ```js
  // On by default.
  await attachHevcSupport(player, { wasmUrl, workerUrl });
  // To tune:    { adaptiveCompute: { targetSpeedX: 1.5 } }
  // To opt out: { adaptiveCompute: false }
  ```

  The Shaka handle remains callable for backwards compatibility
  (`handle()` still unregisters the transmuxer), with `unregister()` /
  `attachComputeAware(player)` exposed as methods on the same handle.

  Both plugins now also re-export `subscribeSegmentStat` and
  `SegmentPerfStat` from `@hevcjs/core` so consumers can plug their own
  telemetry on the perf bus without a separate `@hevcjs/core` dependency.

  Closes [#127](https://github.com/privaloops/hevc.js/issues/127).

## 1.2.1

### Patch Changes

- [#124](https://github.com/privaloops/hevc.js/pull/124) [`186b4ce`](https://github.com/privaloops/hevc.js/commit/186b4ce3fa54c347c1aa1d8e5ddf5ca86a5098b1) Thanks [@privaloops](https://github.com/privaloops)! - Fix Shaka Player playback stalls on HEVC streams with ABR.

  `SegmentTranscoder.prepareInit()` did not reset the per-stream encoder
  state on re-call, so a representation switch (e.g. 480p → 720p) would
  leave the previous `H264Encoder` running while `_width`/`_height` were
  overwritten — new-resolution frames went through the previous encoder
  and MSE rendered garbage from the first switch onward.

  `HevcTransmuxer` now caches the last HEVC init segment it processed and
  short-circuits when Shaka resends the exact same bytes. Without this,
  Shaka's periodic transmuxer re-checks (variant probing, init repeat on
  every segment in some flows) ran the full `prepareInit` pipeline every
  time — closing the live encoder, warming up a throwaway one, and
  forcing the next media segment to rebuild the encoder, which produced
  a visible stall every few seconds. Real representation switches arrive
  with different bytes and still go through the full path.

## 1.2.0

### Minor Changes

- [#108](https://github.com/privaloops/hevc.js/pull/108) [`e96f66b`](https://github.com/privaloops/hevc.js/commit/e96f66b48a167ff3ddd9a4cb53885c7dad34c1f6) Thanks [@privaloops](https://github.com/privaloops)! - Add `prepareInit` to the transcode worker protocol.

  `TranscodeWorkerClient` now exposes `prepareInit(data: Uint8Array): Promise<TranscodedInit>` that mirrors `SegmentTranscoder.prepareInit()` but runs inside the Web Worker. The worker handles the new `{ type: "prepareInit", data, id }` message and replies with `{ type: "initPrepared", id, initSegment, codec }` (transferable ArrayBuffer for the H.264 init segment).

  Required to let transmuxer-style plugins (Shaka Player) hand a synthesized H.264 init segment back to the host player before any media segment has been seen, while keeping the actual HEVC decode + warmup-encode off the main thread.

  No breaking change — purely additive on `TranscodeWorkerClient` and the worker message protocol.

- [#107](https://github.com/privaloops/hevc.js/pull/107) [`0e4bb47`](https://github.com/privaloops/hevc.js/commit/0e4bb47fd91c05dcce08e53bd4235d2b7fd31c63) Thanks [@privaloops](https://github.com/privaloops)! - Add Shaka Player support via the new `@hevcjs/shaka-plugin` package.

  `@hevcjs/shaka-plugin` registers a Shaka `Transmuxer` for `hev1`/`hvc1` mime types that decodes HEVC and re-encodes to H.264 fMP4 via `@hevcjs/core`. The transmuxer exposes the standard `isSupported` / `convertCodecs` hooks so Shaka handles MIME routing natively — applications that want to force the transmuxer even on browsers with native HEVC support can use Shaka's built-in `player.configure({ mediaSource: { forceTransmux: true } })`. The `registerHevcTransmuxer(shaka, config)` second argument forwards `wasmUrl` / `wasmBinaryUrl` (and other `SegmentTranscoderConfig` fields) to the underlying decoder, useful when serving the WASM from a custom path or CDN. Set `workerUrl` to route the HEVC decode + H.264 encode pipeline through a Web Worker (off-main-thread) — recommended for 4K and high-bitrate streams. Bumps the package from a no-op skeleton (0.1.0) to first functional release (0.2.0). Tracks [#101](https://github.com/privaloops/hevc.js/issues/101).

  `@hevcjs/core` exposes a new `SegmentTranscoder.prepareInit()` method that processes an HEVC init segment and immediately returns a matching H.264 fMP4 init segment by warming up the encoder with a single black frame. Required by transmuxer plugins that must hand an init segment back to their host player before any media segment has been seen (Shaka 4.x's `Transmuxer.transmux()` contract).
