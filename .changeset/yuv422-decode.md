---
"@hevcjs/core": minor
---

Add correct Main 4:2:2 (and Main 4:2:2 10) decoding.

4:2:2 was previously broken: the transform unit parsed only one chroma
transform block per luma TB, whereas in 4:2:2 each luma TB maps to TWO chroma
TBs stacked vertically (chroma grid is W/2 x H). This desynced CABAC and read
past the end of the bitstream on any real 4:2:2 stream.

Fixes, all matching the ffmpeg reference bit-exactly:

- `transform_tree` (§7.3.8.8): for ChromaArrayType == 2, parse a second
  `cbf_cb`/`cbf_cr` when `!split_transform_flag || log2TrafoSize == 3`, and
  carry BOTH parent CBF entries (top/bottom) down to the 4x4 leaves so a
  deferred 4:2:2 chroma TU reconstructs both stacked blocks correctly.

- `transform_unit` (§7.3.8.10): process both stacked chroma blocks per
  component (top and bottom), each with its own residual coding, dequant,
  inverse transform and intra/inter prediction; the lower block sits
  `1 << log2TrafoSizeC` chroma rows below the upper one.

- Chroma intra mode mapping (§8.4.3 Table 8-3): remap the derived chroma intra
  mode for the rectangular 4:2:2 sampling grid.

- Intra reference-sample availability (§6.4.1): perform all CTB / Z-scan
  arithmetic in luma sample space. Chroma CTB height is full in 4:2:2
  (SubHeightC == 1), so a chroma row of 32 is still inside a 64-wide luma CTB;
  the previous code divided the vertical CTB index by SubWidthC and wrongly
  marked the top neighbour as belonging to a later CTB.

- Inter chroma motion compensation (§8.5.3.3.2): derive the chroma MV as
  `mvLX * 2 / SubWidthC` (resp. SubHeightC), doubling the vertical component in
  4:2:2 so vertical chroma interpolation runs at full sampling density.

Adds 8-bit and 10-bit 4:2:2 oracle fixtures (QCIF, I + P frames, deblocking +
SAO; byte-identical to ffmpeg). All prior 4:2:0 / monochrome / 8- and 10-bit
oracles stay byte-identical, and the ASan build is clean on the 4:2:2 streams.
