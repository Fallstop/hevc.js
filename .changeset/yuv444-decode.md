---
"@hevcjs/core": minor
---

Add full HEVC 4:4:4 (Main 4:4:4 / Range Extensions, `ChromaArrayType == 3`)
decoding. Chroma is co-sited at full luma resolution, which the previous
code did not handle on two intra paths, both of which desynced CABAC on any
stream containing intra-NxN coding units (i.e. essentially all real 4:4:4
content):

- **`intra_chroma_pred_mode` is signalled per luma PU in 4:4:4** (§7.3.8.5).
  An intra-NxN CU carries four chroma modes (one per 4x4 PU), not the single
  CU-level mode used for 4:2:0/4:2:2. The decoder parsed only one, so every
  NxN CU left three `intra_chroma_pred_mode` syntax elements unread. Each
  chroma PU now derives its DM from its co-located luma PU, with no 4:2:2
  Table 8-3 remap.

- **Chroma is reconstructed at every transform leaf in 4:4:4**, not deferred
  to `blkIdx == 3` as in 4:2:0/4:2:2. Each 4x4 luma leaf of an NxN split has
  its own co-sited 4x4 chroma transform block; deferring both skipped
  reconstruction of three of the four sub-blocks and, when those blocks were
  chroma-coded, left their `residual_coding` bins unread → CABAC desync.

- **32x32 chroma transforms use the 16x16 chroma scaling matrix** (§7.4.5).
  A 32x32 chroma transform block only exists in 4:4:4; the spec defines no
  dedicated 32x32 chroma scaling-list matrix, so it reuses the 16x16 chroma
  matrix and DC. The dequantizer previously read the 32x32 *luma* matrix for
  these blocks, mis-dequantizing 4:4:4 chroma whenever a custom scaling list
  set the two differently. Covered by a direct `perform_dequant` unit test
  (the ffmpeg oracle can't emit custom scaling lists).

Verified byte-exact against the ffmpeg/libx265 reference (native and the
shipped WASM build) across I/P/B frames, SAO, deblocking, 8/10/12-bit, and
qp 6..51, with NxN-heavy content. Adds three 4:4:4 oracle fixtures
(`i_p_qcif_444_4f`, `i_p_qcif_444_10b_4f`, `i_p_qcif_444_hq_4f`). All
existing 4:2:0 / 4:2:2 / monochrome / 10-bit oracles stay byte-identical,
and the WASM decoder (`packages/core/wasm/`) is rebuilt to match.

Note: `transform_skip` remains incorrect for all chroma formats (a
pre-existing decoder limitation, not specific to 4:4:4); it is off by
default in common encoders and is tracked separately.
