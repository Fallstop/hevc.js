---
"@hevcjs/core": minor
---

Parse recovery-point SEI (gradual decoding refresh) so non-IDR random-access
tune-in can report when output is reliable.

SEI NAL units were previously skipped entirely. The decoder now has a minimal
SEI subsystem (`sei_message()` payloadType/payloadSize 0xFF-accumulation per
§7.3.5) that parses `recovery_point` (§D.3.8: `recovery_poc_cnt`,
`exact_match_flag`, `broken_link_flag`) and `user_data_unregistered` (§D.3.7:
16-byte UUID + data), and skips unknown payload types by their size.

On a `PREFIX_SEI` carrying `recovery_point`, the decoder binds the recovery
point to the associated picture's POC and exposes it via the new C API
`hevc_decoder_get_recovery_point()` — cameras commonly use periodic
intra-refresh instead of IDR, so this is how a mid-stream tune-in learns when
its decoded output becomes usable. Pixel output is unchanged (the SEI subsystem
does not perturb decoded samples): all conformance/unit tests pass, including
the 8- and 10-bit pixel-perfect oracle, and a new intra-refresh recovery-point
oracle fixture stays byte-identical to the trusted ffmpeg reference.
