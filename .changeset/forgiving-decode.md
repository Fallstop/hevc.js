---
"@hevcjs/core": minor
---

Forgiving decode for damaged and non-standard streams.

- `feed(data, tag?)`: every output frame carries the `tag` of the chunk that
  delivered its first slice (`frame.tag`, `NaN` when untagged), so frames are
  labelled exactly even when pictures are skipped, dropped or reordered.
  Frames and views also carry `flags` (`HEVC_FRAME_CORRUPT`,
  `HEVC_FRAME_MISSING_REF`, `HEVC_FRAME_PRE_RECOVERY`), and `decoder.stats`
  exposes decode counters.
- `setErrorPolicy({ concealSliceErrors, decodeBeforeIrap })`: conceal lost or
  undecodable slices and missing references instead of freezing until the
  next IRAP, and decode pictures before the first IRAP (opt-in; the default is
  unchanged and bit-exact).
- `discontinuity()`: declare a splice/seek/gap without resetting the decoder.
- Hardening: spec-range validation of parameter sets, slice headers and CABAC
  values; corrupt parameter sets and SEI are contained; full VUI/HRD parsing;
  pictures are assembled across interleaved SEI/filler and across feed()
  calls; per-slice reference lists; bounded DPB; output order fixed when
  draining per picture; `pic_output_flag` honoured; a 4:2:2/4:4:4 chroma
  interpolation stack overflow and a WPP thread-pool hang fixed.
- Faster: native WebAssembly exception handling, recycled picture buffers,
  32-bit dequantization, and bulk plane/I420 copies in JS.
