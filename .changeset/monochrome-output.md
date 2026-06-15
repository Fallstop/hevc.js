---
"@hevcjs/core": patch
---

Fix monochrome (4:0:0) decoding and output (IR/thermal cameras).

Two monochrome-specific bugs are fixed:

- CABAC desync: in `transform_tree`, `cbf_cb`/`cbf_cr` are never parsed for
  monochrome (ChromaArrayType == 0), so they kept inheriting the CU root's
  `(true, true)` seed. The spurious chroma CBF then satisfied the
  `cu_qp_delta` read condition (§7.3.8.10) on transform units that have no
  coded coefficients, reading a phantom `cu_qp_delta` and desyncing the CABAC
  engine for the rest of the slice (luma reconstructed as prediction-only).
  Chroma CBF is now forced to 0 for monochrome.

- Output API: `SubWidthC`/`SubHeightC` reported 2/1 for MONOCHROME instead of
  the spec value 1/1 (Table 6-1), and `hevc_decoder_get_frame()` /
  `hevc_decoder_get_drained_frame()` then handed back non-zero chroma
  dimensions and pointers into the zero-length chroma planes. Monochrome
  frames now report 0-width/0-height chroma and NULL `cb`/`cr` pointers.

Adds a monochrome oracle fixture (i400 QCIF, byte-identical to the ffmpeg
reference) and a unit test covering the output-API dimensions. All other
8- and 10-bit pixel-perfect oracles stay byte-identical.
