---
"@hevcjs/core": patch
---

Cut the per-edge metadata overhead in the HEVC deblocking filter — the dominant
cost at 4K, well above the filter arithmetic itself. Two picture-constant
quantities are now derived once per frame instead of per 4-sample edge segment:

- **Reference-picture POCs.** The boundary-strength derivation (§8.7.2.4.5)
  compares pictures by POC; it previously chased `dpb->ref_pic_listX(idx)->poc`
  (vector + `Picture*` indirections) for every inter edge. POCs are constant for
  the whole deblocking pass, so they are precomputed into small flat arrays and
  indexed by ref index.
- **Tile / slice boundary exclusion.** For the common case (no tiles, a single
  slice, deblocking enabled) only the picture boundary can exclude an edge, so
  `is_boundary_excluded` takes a fast path that skips the per-segment CTB-address,
  slice-header, tile and slice-boundary work. The full path is unchanged and is
  selected whenever the picture actually uses tiles, multiple slices, or
  per-slice deblocking/across-slice flags.

Together these drop `apply_deblocking` instructions ~15% and its branch
mispredictions ~31% at 4K (native callgrind). Bit-exact: all 103 conformance/
unit tests pass (incl. 8- and 10-bit oracle) and the shipped WASM output SHA-256
is byte-identical.
