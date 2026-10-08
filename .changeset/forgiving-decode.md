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
- `flushViews()`: zero-copy `flush()`. Output now follows §C.5.2 exactly, so
  `drain()` holds up to `sps_max_num_reorder_pics` pictures for reordering;
  `SegmentTranscoder` and `TranscodePipeline` flush at each segment end (the
  references are kept) so every segment still yields all of its frames.
- Hardening: spec-range validation of parameter sets, slice headers and CABAC
  values; corrupt parameter sets and SEI are contained; full VUI/HRD parsing;
  pictures are assembled across interleaved SEI/filler and across feed()
  calls; per-slice reference lists; bounded DPB; output order fixed when
  draining per picture; `pic_output_flag` honoured; a 4:2:2/4:4:4 chroma
  interpolation stack overflow and a WPP thread-pool hang fixed.
- Conformance: tile and WPP substreams, dependent slice segments, PCM, the
  parallel merge level, merge pruning, constrained intra prediction and POC
  derivation fixed. Of the 147 JCT-VC HEVC_v1 streams, 139 now match ffmpeg
  (58 before, with 6 hangs) and 4 more match the suite's own MD5 where ffmpeg
  is wrong; the last 4 differ by design: pictures waiting for output at an
  IRAP with NoOutputOfPriorPicsFlag are output, not discarded.
- Faster: native WebAssembly exception handling, recycled picture buffers,
  32-bit dequantization, and bulk plane/I420 copies in JS.
