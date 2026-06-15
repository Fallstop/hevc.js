---
"@hevcjs/core": minor
---

Native 8-bit (uint8) pixel path — store and process 8-bit HEVC at one byte per
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
