---
"@hevcjs/core": minor
---

Add a cheap decoder `reset()` and a zero-copy frame-output path.

- **`HEVCDecoder.reset(clearParameterSets = true)`** — reset the decoder to its
  initial state and decode a new, independent stream on the *same* WASM instance,
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
