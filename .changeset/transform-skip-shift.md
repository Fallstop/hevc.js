---
"@hevcjs/core": patch
---

Fix `transform_skip` residual scaling for all chroma formats (spec §8.6.4.2).

The transform-skip inverse used a residual shift of `15 - BitDepth`, omitting
the `- Log2(nTbS)` term. The correct net shift is
`15 - BitDepth - Log2(nTbS)` — the skip left-shift `tsShift = 5 + Log2(nTbS)`
combined with the common residual right-shift `bdShift = 20 - BitDepth`
(`log2TransformRange = 15`, Main profile). The decoder therefore over-shifted
by `Log2(nTbS)` and reconstructed every transform-skipped block at 1/4 of its
correct amplitude (4x4 is the only skip size in the supported profiles). This
affected 4:2:0, 4:2:2 and 4:4:4 alike; it went unnoticed because
`transform_skip_enabled_flag` is off in common encoder presets and no fixture
exercised it.

Verified byte-exact vs the ffmpeg/libx265 reference across 4:2:0 / 4:2:2 /
4:4:4 at 8/10/12-bit. Adds two transform-skip oracle fixtures
(`i_p_qcif_tskip_4f`, `i_p_qcif_444_tskip_4f`), both of which decode wrong
before the fix.
