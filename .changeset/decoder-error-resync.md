---
"@hevcjs/core": minor
---

Contain per-picture decode failures and resynchronize at the next IRAP, instead
of aborting the whole feed() buffer — robustness for 24/7 lossy camera streams.

Previously a single bad/undecodable picture (truncated/corrupt VCL payload)
aborted the entire `feed()`/`decode()` buffer, and a bitstream over-read thrown
on a WPP worker thread escaped uncaught and called `std::terminate()`, crashing
the process. Now a per-picture failure is contained: the half-decoded picture is
dropped from the DPB, the decoder skips P/B pictures until the next IRAP
(mirroring the §8.1 wait-for-random-access-point behaviour, but re-armed by a
decode error), and output recovers byte-identically from the next keyframe.

Specifically: `ThreadPool` worker jobs no longer let exceptions escape the
worker thread; the WPP row decoder publishes row completion even after a thrown
failure so dependent rows never deadlock, and reports failure out-of-band so the
picture is discarded; the top-level decode loop catches per-picture failures and
arms skip-to-next-IRAP rather than returning an error; and `BitstreamReader`
read-width validation is a catchable `throw` (not a hard `assert` that would
abort in debug/ASan builds) so the same resync path works in every build.

Pixel output is unchanged on well-formed streams: all conformance/unit tests
pass, including the 8- and 10-bit pixel-perfect oracle, plus new gtests that
corrupt a VCL NAL in a multi-GOP stream and assert the decoder does not crash,
does not abort the buffer, and recovers byte-exact output at the next IRAP.
