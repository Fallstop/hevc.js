---
"@hevcjs/core": patch
---

Harden the decoder against malformed, truncated, and lossy bitstreams — the
common reality of 24/7 IP-camera (RTSP/RTP) streams and mid-stream joins. These
are robustness/memory-safety fixes with no change to conformant decode output;
all conformance/oracle tests stay green (8- and 10-bit) and the AddressSanitizer
build is clean.

- **No more decoder hang on a reference-less P/B slice.** Reference-picture list
  construction could spin forever when the active RPS declared no usable
  references (a truncated/corrupt inline RPS, or a non-I slice fed before its
  references exist). The temp-list build now always makes progress, eliminating
  the unrecoverable playback freeze.
- **Bounds-checked reference-list modification.** `list_entry_l0/l1` indices and
  `num_ref_idx_l*_active_minus1` from the bitstream are now clamped, fixing an
  out-of-bounds read / wild-pointer dereference and a potential stack overflow
  on crafted slice headers.
- **Graceful concealment instead of garbage on a missing reference.** When a
  referenced picture is absent (packet loss / mid-GOP join), motion
  compensation previously blended uninitialised memory into the output; it now
  drops the missing prediction and conceals an otherwise-unpredictable block as
  neutral mid-grey rather than emitting garbage (and removes the undefined read).
- **SPS dimension/conformance-window validation.** Picture dimensions and crop
  offsets parsed from the SPS are now validated (non-zero, within a sane level
  cap, a multiple of the minimum coding-block size, crop within the picture);
  an invalid SPS is rejected instead of flowing unchecked into allocation and
  the SIMD filter bounds.
