---
"@hevcjs/core": minor
---

Harden the decoder's frame-output API for production use.

- **`HEVCFrameView` lifetime guards** — zero-copy views from `drainViews()` now
  carry `isValid()` and `assertValid()`. Each view captures the decoder's
  view-generation counter and the heap `ArrayBuffer` it aliases; the counter is
  bumped at the start of every heap-touching call (`decode` / `feed` / `drain` /
  `drainViews` / `flush` / `reset` / `destroy`). A stale view (a later decoder
  call) or a detached buffer (memory growth under `ALLOW_MEMORY_GROWTH`) now
  fails loudly with a clear `Error` instead of silently reading zeros/garbage.
  The synchronous transcode/segment-transcoder fast path is unchanged.

- **Discriminated frame types** — `HEVCFrame` and `HEVCFrameView` are now
  discriminated unions on `bytesPerSample: 1 | 2`, so consumers narrow the
  `Uint8Array` / `Uint16Array` planes by checking `bytesPerSample` (no non-null
  `!` asserts). `HEVCFrameView` is now exported from the package entry point.

- **Single ABI definition** — the 48-byte WASM `HEVCFrame` struct offsets and
  the plane-copy helper, previously duplicated across `decoder.ts` and
  `worker.ts`, are now defined once in `frame-layout.ts`.

No decode-output change; behavior is identical.
