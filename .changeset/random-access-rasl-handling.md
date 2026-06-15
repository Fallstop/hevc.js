---
"@hevcjs/core": minor
---

Add HEVC random-access handling so the decoder can correctly tune in to a
stream mid-GOP — the normal case for security cameras (which commonly use CRA
random-access points) and for players that seek or join a live stream.

- Derive `NoRaslOutputFlag` per IRAP at the decoder level (IDR/BLA always; CRA
  when it is the first picture of the bitstream, the first after an end-of-stream,
  or the first after `reset()`), and drive the POC/RPS reset from it so a
  mid-stream CRA is treated as a random-access point rather than mishandled.
- Discard the associated RASL leading pictures of an IRAP with
  `NoRaslOutputFlag == 1` (their references precede the random-access point and
  are unavailable), instead of decoding them against absent references and
  emitting corrupt frames.
- After a fresh start or `reset()`, skip non-IRAP pictures until the first
  random-access point, rather than attempting to decode trailing P/B pictures
  whose references do not exist yet.

Conformant full-GOP-from-IDR streams are unaffected; output for the existing
oracle suite is byte-identical. Verified bit-exact against an open-GOP (CRA +
RASL) fixture.
