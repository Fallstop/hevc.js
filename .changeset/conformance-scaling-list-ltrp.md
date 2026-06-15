---
"@hevcjs/core": patch
---

Two HEVC conformance fixes for paths the prior fixtures did not exercise. No API
change; all conformance/oracle tests pass, with new targeted tests added.

- **Explicit scaling lists are now applied in the correct order.** Custom
  (signalled) quantisation matrices were stored in up-right diagonal scan order
  but read back in raster order, scrambling the coefficients and producing
  blocky/garbage output for any stream that signals `*_scaling_list_data_present`.
  Parsed coefficients are now stored through the inverse diagonal scan so the
  dequant read is correct (default scaling lists were already correct).
- **SPS-indexed long-term reference pictures are now resolved.** Long-term
  references signalled by index into the SPS candidate list (`lt_idx_sps`) left
  their POC LSB and "used by current picture" flag unresolved, silently dropping
  the reference. They are now resolved from the SPS-declared candidates per
  §7.4.7.1 (the inline long-term form was already handled).
