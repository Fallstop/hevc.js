---
"@hevcjs/core": patch
---

Fix chroma QP derivation for 4:2:2 / 4:4:4 (spec §8.6.1).

The chroma QP (`qPc` from `qPi`) was always mapped through the 4:2:0
chroma-QP table (Table 8-10). Per §8.6.1 that mapping applies only for
`ChromaArrayType == 1` (4:2:0); for `ChromaArrayType == 2` (4:2:2) and
`3` (4:4:4) `qPc = Min(qPi, 51)` (a plain clamp, no table). The table is
the identity for `qPi <= 29`, so low-QP 4:2:2 streams decoded correctly
by accident, but any 4:2:2/4:4:4 stream with `qPi > ~30` reconstructed
chroma with the wrong quantizer.

While fixing this, the chroma inter-prediction buffers (`cpred`) in
`decode_coding_unit` were sized `[32*32]`, correct only for 4:2:0. In
4:2:2 (chroma 32x64) and 4:4:4 (chroma 64x64) large CUs this overran the
stack buffer in the SIMD store path of `weighted_pred_default`. The
buffers are now sized `[64*64]` to cover all chroma formats.

Adds a high-QP (qp=42) 4:2:2 oracle fixture that exercises the divergent
clamp path and is byte-identical to the ffmpeg reference. All existing
4:2:0 and low-QP 4:2:2 oracles stay byte-identical.
