# Architecture Decisions

## AD-001 : Bit depth — Runtime, pas templates

**Contexte** : Le plan mentionne "templates pour le bit depth (8/10/16)". Deux approches possibles :
- Template `<int BitDepth>` : specialisation compile-time, performance max, code duplique
- Runtime `int bit_depth` : un seul chemin de code, plus simple, branchement runtime

**Decision** : Runtime avec `uint16_t` comme type de sample universel.

**Justification** :
- Le Main profile (Phase 4-6) est 8-bit only. Pas besoin de templates avant Phase 7
- `uint16_t` contient 8-bit et 10-bit sans surcharge
- La performance viendra du SIMD (Phase 9), pas des templates
- Moins de code = moins de bugs de conformite

**Consequence** : `Pixel = uint16_t` partout. Les shifts/clips utilisent `BitDepth` en runtime.

## AD-002 : Picture layout — Planar (separate planes)

**Decision** : 3 buffers separes Y, Cb, Cr avec stride independant.

**Justification** :
- La spec traite chaque composante independamment
- Le deblocking et SAO operent plan par plan
- L'interpolation chroma a des dimensions differentes
- Les filtres SIMD preferent les donnees contiguees par plan

**Structure** :
```cpp
struct Picture {
    std::vector<uint16_t> planes[3];  // Y=0, Cb=1, Cr=2
    int width[3], height[3];           // dimensions par plan
    int stride[3];                     // stride par plan (en samples)
    int bit_depth;
    int chroma_format_idc;             // 1=420, 2=422, 3=444
    int poc;
};
```

## AD-003 : Parameter set ownership — Stored by value, indexed by ID

**Decision** : `std::array<std::optional<VPS>, 16>` pour les VPS, idem SPS (16 max), PPS (64 max).

**Justification** :
- Les IDs sont bornes par la spec (VPS: 0-15, SPS: 0-15, PPS: 0-63)
- Pas besoin de map dynamique
- Copie complete a chaque mise a jour (les PS sont petits)
- Le slice header reference le PPS actif par ID

## AD-004 : Error handling — Assertions en debug, codes d'erreur en release

**Decision** :
- Debug : `assert()` + `HEVC_DEBUG` logging pour tracer les valeurs intermediaires
- Release : fonctions retournent `bool` ou `enum HEVCError`
- Pas d'exceptions C++ (requis pour l'API C WASM)

## AD-005 : CABAC — Struct of arrays pour les contextes

**Decision** : Tous les contextes dans un seul tableau flat, indexes par ctxIdx global.

```cpp
struct CabacContext {
    uint8_t pStateIdx;
    uint8_t valMps;
};

struct CabacEngine {
    CabacContext contexts[256];  // tous les contextes
    uint16_t ivlCurrRange;
    uint16_t ivlOffset;
    BitstreamReader* bs;
};
```

**Justification** :
- Acces O(1) par ctxIdx
- Init simple (boucle sur le tableau)
- Save/restore pour WPP en un memcpy

## AD-006 : Intermediate buffers — Stack-allocated, taille max CTU

**Decision** : Les buffers temporaires (prediction, residual, transform) sont alloues sur la stack avec la taille max du CTU (64x64).

```cpp
// Dans la fonction de decodage d'un CU :
int16_t pred_samples[64 * 64];
int16_t residual[64 * 64];
int32_t transform_buf[64 * 64];  // precision etendue pour transform
```

**Justification** :
- Pas d'allocation dynamique dans le hot path
- 64*64*4 = 16KB par buffer, ~48KB total — acceptable sur la stack
- WASM : la stack par defaut Emscripten est ~64KB (le heap est extensible, pas la stack). Prevoir `-sSTACK_SIZE=1048576` en Phase 8, ou restructurer pour allouer les buffers au niveau CTU

## AD-010 : Upstream merge (privaloops/hevc.js core 1.4.0..1.4.8) — re-implemented, not merged

*English, per the repo's documentation convention. Upstream's own AD-007..AD-009
are not copied here; the numbering skips them to avoid a collision.*

**Context**: origin/main is 281 commits past our merge base (b32f001). Its decoder
fixes touch the same functions this fork rewrote (decoder-level NoRaslOutputFlag,
uint8 planes, 4:2:2/4:4:4, reset(), recovery-point SEI), so a git merge or a
cherry-pick conflicts everywhere and would also bring upstream's rejection of
4:2:2/4:4:4 SPSs.

**Decision**: Re-implement each decoder correctness fix against this fork, with
tests, and record the disposition of every upstream item:

| Upstream fix | Disposition |
|---|---|
| Hang on P/B slices with zero references (#248) | Already guarded in `DPB::build_ref_pic_list` (all-empty RPS breaks out, list conceals to null). The parser-side rejection of `NumPicTotalCurr == 0` is not ported: a forgiving decoder conceals that picture instead of dropping it. |
| Out-of-range `list_entry_lX` | Rejected in the slice header (P0, AD-011); the DPB keeps its null-conceal guard. |
| Missing references read uninitialised prediction buffers | Already in the fork (`perform_inter_prediction` drops the predFlag and conceals grey). Ported the 16-bit clamp of the grey value. |
| Long-term refs signalled in the SPS dropped (#249) | Already in the fork (`test_slice_ltrp_sps`). Ported the missing `DeltaPocMsbCycleLt` accumulation (eq 7-52) and the inferred zeros for `lt_idx_sps` / `delta_poc_msb_cycle_lt`. |
| `pic_output_flag` honoured (§C.3.1) | Ported: the flag is no longer overwritten with 1 after decoding; batch output skips such pictures. Upstream's `take_suppressed_pictures()` API is **not** ported: tagged output (AD-014) gives the caller exact per-frame timestamps, which supersedes positional re-mapping. |
| RASL suppression after an opening CRA (§8.1) | Already in the fork as a skip (RASL of an IRAP with NoRaslOutputFlag = 1 is never decoded). Upstream's fixtures and MD5 oracles (`opengop_qcif_12f`, `opengop_qcif_cra_first`) are ported and pass unchanged. |
| Reference lists built on I slices too (heap-use-after-free on the TMVP ref-POC snapshot after an IRAP) | Ported; `construct_ref_pic_lists` clears both lists before any early return, and `derive_colpic` runs on I slices to clear ColPic. |
| Negative / oversized ref index in `ref_pic_list0/1()`, `num_ref_idx` int overflow | Ported. |
| Bounded decoder memory during transcode / drain order | Re-implemented rather than ported. Upstream excludes the just-decoded picture from `drain()`'s §C.5.2.2 counts, which delays every frame of a low-delay stream by one picture — unacceptable for live view. This fork bumps at the two points §C.5.2 defines (§C.5.2.2 before the current picture is stored, §C.5.2.3 after it is decoded, including PicLatencyCount) into an output queue that `drain()`/`flush()` hand over. Output order is spec-exact for any drain cadence, and `sps_max_num_reorder_pics = 0` streams come out in the same call that decoded them. |
| SPS/PPS/slice-header bounds (4d09a13) | Ported into the P0 validation work (AD-011), **minus** the 4:2:2/4:4:4 rejection (this fork decodes both) and with the invalid conformance window zeroed instead of rejected (AD-012). |
| `HEVCFrame.cvs_id` | Not ported: tags (AD-014) carry the caller's own timestamp, which is what the field was for. |
| TS changes (DisplayPtsAssigner, transcode pipeline, dash/shaka plugins) | Not ported (out of scope for this fork's consumer). |
| Perf: SAO per-CTB uniform test, 32-bit dequant, shift-indexed grids | Evaluated separately in the perf pass (AD-017). |

**Consequence**: `prior pictures` are never discarded at an IRAP with
NoRaslOutputFlag = 1, whatever `no_output_of_prior_pics_flag` says — every frame
the caller fed comes back out (upstream and the previous code behaved the same).
`DPB` storage is additionally capped at 16 pictures (P0).

## AD-011 : Spec-range validation — refuse the NAL, never index with it

**Context**: A 720-run fuzz smoke found stack/heap overflows from header values
the parser trusted (inline RPS with more than 16 pictures, `slice_segment_address
>= PicSizeInCtbsY`, `sps_max_sub_layers_minus1 == 7`), unbounded `resize()` on
ue(v) counts, Exp-Golomb prefixes shifted past 31 bits, and `seek_to_byte`
asserts on corrupt entry points.

**Decision**: Every syntax element that sizes an allocation, indexes a
fixed-size array, or feeds a shift is range-checked against its §7.4 bound
where it is parsed, and the parameter set / slice header is refused (`return
false`). Data errors met inside slice data (Exp-Golomb prefix over 32 bins, a
`CuQpDeltaVal` outside ±(26 + QpBdOffsetY/2), an initial CABAC offset of 510/511)
throw, which fails the slice exactly like a bitstream over-read.

**Rationale / boundaries**:
- Refusing is the memory-safe default; whether the picture is then dropped or
  concealed is the error policy's business (AD-016), not the parser's.
- Fields that only steer output timing are clamped instead (the SPS
  `max_dec_pic_buffering_minus1` to 15 and `max_num_reorder_pics` to it): a
  camera that overstates them still decodes.
- Bit depths above 12 are refused: without extended_precision_processing the
  interpolation/weighting shifts go negative. Range-extension PPS tools this
  decoder does not implement (cross-component prediction, chroma QP offset
  lists, transform skip above 4x4, SAO offset scaling) are refused rather than
  silently mis-decoded.
- `seek_to_byte` clamps to the end of the buffer; the next read throws.
- The DPB never stores more than 16 pictures: §C.5.2.2 bumping runs first, and
  if a corrupt RPS still pins more than 16 references the oldest are unmarked.
- `NumPicTotalCurr == 0` on a P/B slice is *not* refused (upstream refuses it):
  the list builder already terminates and the PUs conceal, which keeps the
  picture.

## AD-012 : Non-VCL failures are contained; the previous valid set stays in force

**Context**: `Decoder::decode` caught exceptions per picture, but parameter-set
and SEI processing sat outside that `try`: a truncated SPS threw out of
`feed()` and the rest of the buffer was lost. The VUI skipper stopped at HRD
parameters and then read the SPS extension flags out of HRD data. An invalid
conformance window rejected the whole SPS. A PPS parsed against one SPS kept
its tile tables when that SPS was replaced by one of another size (out-of-bounds
CTB-address lookups).

**Decision**:
- VPS/SPS/PPS/SEI processing is wrapped in `try`; a set that fails to parse is
  counted (`nal_errors`) and dropped. Sets are only stored once fully parsed, so
  the previous valid set with that id remains active.
- `hrd_parameters()` / `sub_layer_hrd_parameters()` (§E.2.2/E.2.3) and the full
  VUI (§E.2.1) are parsed, for the SPS and the VPS. If the VUI does not parse —
  over-read or out-of-range value — it is retried with the alternative layout
  some encoders write (default display window flag set, offsets absent, as
  libavcodec does); if that fails too the SPS is kept with the extension flags
  at their inferred 0 (`SPS::vui_tolerated`). The extension flags are also
  ignored when the RBSP does not end where they do, i.e. when they are really
  mis-read VUI bits.
- An invalid conformance window is zeroed (no cropping), not rejected.
- The PPS syntax is parsed independently of the SPS (§7.3.2.3 has no SPS
  dependency). SPS-dependent checks and the tile tables are derived against the
  SPS *generation* the PPS was last derived for; storing an SPS bumps its
  generation, and activation re-derives any PPS whose generation is stale. A
  PPS that arrives before its SPS is therefore kept, and an SPS replacement
  never leaves stale tables.

**Consequence**: a broken VUI can no longer cost a stream, but a stream whose
range-extension flags sit after a broken VUI loses them — the safe direction
for Main/Main 10 cameras.

## AD-013 : Pictures are assembled slice by slice, across non-VCL NALs and feed() calls

**Context**: VCL NALs were grouped into a picture only while adjacent, so a
SUFFIX_SEI or filler NAL between two slices split the picture in two; reference
lists and ColPic were built once from the first slice; dependent slice segments
lost their entry points and inherited from `slice_headers[0]`; a slice of the
next picture whose first slice was lost was decoded *into* the current one; a
picture had to arrive whole in one `feed()`; and reserved VCL types,
`nuh_layer_id > 0` and forbidden-bit NALs were decoded.

**Decision**:
- A pending picture is assembled one slice segment at a time. A segment
  continues it when it is not a first segment, shares its PPS and POC LSB, and
  starts at a tile-scan address past the previous segment's *start* (a failed
  segment claims only its start CTB, so the segment after it still fits and
  re-decodes the CTBs it overran). Repeated and stray segments are dropped
  (AD-019); anything else finishes the pending picture first. The picture is
  also finished as soon as its segments reach the last CTB (so
  per-access-unit feeding outputs in the same call), at AUD/EOS/EOB, and at
  `flush()`/batch output. A VPS/SPS/PPS arriving while a picture is pending is
  held back and applied once it finishes (AD-019). Prefix SEI, suffix SEI,
  filler and reserved/unspecified non-VCL NALs never split a picture. A
  picture may therefore span `feed()` calls (NAL-at-a-time input).
- Reference lists and ColPic are built per independent slice. Each slice's
  lists are snapshot (POC + long-term flag) into the picture, with a per-CTB
  slice map when there is more than one: deblocking compares references
  through the table of each block's own slice, and TMVP reads the collocated
  block's slice table — including the §8.5.3.2.9 long-term rules (no
  long-term/short-term mixing, no scaling between long-term references).
- Dependent segments parse their own entry points / extension / alignment,
  inherit from the independent segment of *their* slice, share its slice
  number (SliceAddrRs) for availability, SAO merge and filters, and follow the
  §9.3.1 context rules (initialize at a tile start, WPP-synchronize at a row
  start when the CTB above-right is in the same slice, otherwise continue the
  previous segment). A dependent segment whose predecessor is missing or failed
  is dropped (orphan). The row-parallel WPP path is limited to independent
  segments starting at a row start and to the segment's own rows (it used to
  decode every row to the bottom of the picture from the wrong data, and
  deadlocked on one-CTB-wide pictures).
- Reserved VCL types, `nuh_layer_id > 0` (all NAL types) and NALs with
  `forbidden_zero_bit` set or `nuh_temporal_id_plus1 == 0` are not decoded; the
  last two count as `nal_errors`.
- References whose picture size, chroma format or bit depth differ from the
  current SPS are treated as missing (their planes and motion grid do not fit).

**Verification**: besides the unit tests, the JCT-VC HEVC_v1 suite compared
against ffmpeg (`tools/jctvc_conformance.py`): 58/147 → 67/147 matching, no
regression; the baseline hung (≥900 s) on CONFWIN_A, FILLER_A, INITQP_A/B and
WPP_D, all of which now complete. The remaining mismatches predate this work
and are outside it (see the report accompanying this branch).

## AD-014 : Tagged output — the caller labels frames, the decoder carries the label

**Context**: callers paired output frames with input timestamps by counting.
Every picture the decoder skips (waiting for an IRAP, RASL, error resync), drops
(decode error) or does not output (`pic_output_flag = 0`) shifts every later
frame onto the wrong timestamp. Upstream answered with a report of suppressed
(cvs_id, poc) pairs the caller must replay (its AD-009); that still leaves the
caller re-deriving the decoder's decisions.

**Decision**: `hevc_decoder_feed_tagged(dec, data, size, double tag)` — every
picture whose first slice segment arrives in that call carries `tag` to output
(`HEVCFrame.tag`, after `poc`/`bytes_per_sample`, at offset 48 on wasm32; the
struct grows from 48 to 64 bytes). `hevc_decoder_feed` keeps working and tags
NaN. Frames also carry `flags` (`HEVC_FRAME_CORRUPT`, `_MISSING_REF`,
`_PRE_RECOVERY`), and `hevc_decoder_get_stats` exposes running counters
(decoded, output, skipped waiting for IRAP / RASL / error, concealed, contained
non-VCL errors). The JS wrapper's `feed(data, tag?)` always goes through the
tagged entry point; frames and views gain readonly `tag` and `flags`.

**Rationale**: the tag is a double so a JS timestamp (µs, 90 kHz ticks or
seconds) round-trips exactly. A picture spanning feed calls is tagged by the call
that carried its first slice — the one whose sample/AU it belongs to. Counters
are cumulative `uint32` (reset by `reset()`); the caller diffs them.

**Consequence**: `MISSING_REF` is now visible under the default policy too: a
picture whose RPS names a picture the decoder never saw (a whole access unit
lost) is decoded with neutral-grey prediction where it references it, and says so.

## AD-015 : discontinuity() — a cut in the input, not a new decoder

**Context**: a live feed with a gap, a splice of archive clips or a seek within
one stream needed `reset(false)`, which also throws away every decoded picture
not yet output and the decoder's statistics, and still relies on the caller to
know that the next CRA's RASL pictures cannot be decoded.

**Decision**: `hevc_decoder_discontinuity(dec)` / `discontinuity()`:
- finishes the picture being assembled;
- marks every stored picture unused for reference and drops the decoder's
  last-picture concealment fallback; the DPB records the cut in decoding
  order and never uses a picture from before it as a concealment source — so
  nothing fed afterwards is predicted from, or patched with, a picture before
  the call, whatever the error policy;
- bumps every picture awaiting output into the output queue (the next `drain()`
  returns them in output order — nothing decoded is lost);
- re-arms the §8.1 tune-in state: the next CRA gets NoRaslOutputFlag = 1 (as
  HandleCraAsBlaFlag would), and non-IRAP pictures before the next IRAP are
  skipped (`skipped_wait_irap`) unless the error policy decodes them;
- keeps parameter sets, scratch allocations, POC carry-over and statistics.

## AD-016 : Forgiving error policy — opt-in, conceal and keep going

**Context**: a security-camera client prefers a damaged but moving picture to a
frozen one. The decoder dropped any picture with an error and skipped every
non-IRAP picture until the next IRAP (seconds of freeze on cameras with long
GOPs), skipped everything before the first IRAP, and predicted from mid-grey
where a reference was missing.

**Decision**: `hevc_decoder_set_error_policy(dec, flags)` / `setErrorPolicy({...})`.
The default (0) keeps the previous behaviour, so conformance output and every
MD5 oracle are unchanged.
- `HEVC_POLICY_CONCEAL_SLICE_ERRORS`: when the first slice header of a picture
  decoded (POC, RPS, output order known) but a slice fails, is missing, or the
  slices do not cover the picture, the undecoded CTUs are filled with the
  co-located samples of the reference picture nearest in POC (same CVS;
  otherwise the most recently decoded one — the previous picture is kept alive
  for this even after an IDR released it; mid-grey only if nothing fits; never
  a picture from before a `discontinuity()`). Only reference pictures qualify:
  a non-reference picture stays in storage until the caller drains it, and
  using it would make the concealed samples depend on drain timing. Their SAO parameters, motion and
  every deblocking edge inside or on the boundary of the area are cleared, so
  the filters neither run inside the concealed area nor across its seam. The
  picture is kept as a reference and output with `CORRUPT`; skip-to-IRAP is
  armed only when the first slice header itself is undecodable. A missing
  reference picture is replaced by the stored picture nearest to the POC the
  RPS names (`MISSING_REF`); the list entry keeps the *named* POC so MV
  scaling and reference comparisons follow the bitstream, and a substituted
  ColPic contributes zero temporal motion (the lost picture's motion field is
  unknowable; zero is right for the static scenes cameras mostly see). A list
  entry with no picture at all (an RPS naming nothing current, a `list_entry`
  past the candidates) is filled the same way, entry i of list 0 (1) taken to
  name the picture i+1 before (after) the current one; under either policy
  such a slice flags its picture `MISSING_REF`.
- `HEVC_POLICY_DECODE_BEFORE_IRAP`: non-IRAP pictures at stream start, after
  `reset()`/`discontinuity()` or after an undecodable picture are decoded
  (missing references concealed as above) and flagged `PRE_RECOVERY` until an
  IRAP, or until the POC of a recovery point SEI (§D.3.8) bound during that
  span — which makes gradual-decoding-refresh cameras decodable at all. RASL
  pictures of an IRAP that opens the sequence stay skipped: they would be
  output *before* that IRAP.

Also, independent of the policy: a slice whose CABAC read position runs well
past the end of its data (zero-padded reads) or that runs past the last CTB
without `end_of_slice_segment_flag` is now an error — a truncated slice used to
decode garbage silently and be output as clean.

**Measured** (camera-like fixture, one access unit lost): luma SAD of the next
picture against the clean decode 591k with mid-grey prediction, 85k with
substitution (~3 per sample, about the freeze error).

## AD-017 : Performance pass — native EH, recycled pictures, upstream hot-path ports

**Context**: the forgiving-decode work adds per-slice bookkeeping (reference
tables, assembly state, validation) to the hot path, and the task allowed no
unexplained regression above 2% at 1080p or 4K. Two in-house perf branches
existed: `Fallstop/perf/simd-wasm-decode` and `feat/performance-focus`.

**Decision**:
- `HEVC_WASM_NATIVE_EH` (default ON): `-fwasm-exceptions` with the legacy EH
  proposal (Chrome 95+, Firefox 100+, Safari 15.2+, below the SIMD floor the
  build already requires) instead of JS-emulated exceptions, which route every
  call that may throw through an `invoke_*` trampoline. The decoder relies on
  exceptions to contain corrupt NALs, so they stay; their cost does not.
- Released pictures go to a two-entry pool and the next picture of the same
  format takes over their sample and motion buffers (no zero-fill: every sample
  is written by decoding or concealment). Pictures still referenced elsewhere
  (`use_count() > 1`, e.g. the concealment fallback) are never recycled.
- Ported from upstream: shift-indexed CU/intra-mode grids and 32-bit
  dequantization when `qpPer <= bdShift` (the 64-bit path now clips before
  narrowing). Not ported: upstream's per-CTB uniform-SAO test, which assumes a
  single slice per picture.
- JS: one `TypedArray.set` per contiguous plane; 8-bit I420 packing by rows.
- `Fallstop/perf/simd-wasm-decode` is already fully contained in this branch's
  base. From `feat/performance-focus` the picture pool, the release
  `ASSERTIONS=0` (already in base) and the JS copies were taken; its scalar SAO
  fast paths and row-pointer interpolation were written against the pre-SIMD
  code and are superseded by the SIMD SAO/interpolation already in base.

**Measured** (Node 24, emscripten 6.0.10, `-O3 -msimd128`, Apple M3;
median of three alternating rounds, each the median of 10 decodes after 2
warm-up; `tools/bench_wasm_stats.mjs` batch, `tools/bench_wasm_stream.mjs`
one access unit per feed with a drain after each):

| Stream | Mode | 6b390dc | this branch | Change |
|---|---|---|---|---|
| bbb1080_50f | batch | 612.0 ms (81.7 fps) | 582.7 ms (85.8 fps) | -4.8% |
| bbb1080_50f | streaming | 609.0 ms (82.1 fps) | 592.3 ms (84.4 fps) | -2.7% |
| bbb4k_25f | batch | 1132.7 ms (22.1 fps) | 1093.7 ms (22.9 fps) | -3.4% |
| bbb4k_25f | streaming | 1130.9 ms (22.1 fps) | 1080.8 ms (23.1 fps) | -4.4% |

`tools/bench_wasm.mjs` (best of 3 per round): 1080p 81.7/81.9/82.3 fps before,
85.5/85.9/86.6 after; 4K 22.0/21.8/22.1 before, 21.6/22.8/23.1 after. Output is
byte-identical between the WASM build and the native CLI on all 38 fixtures.
`hevc-decode.wasm` grows from 234,546 to 254,968 bytes (validation, VUI/HRD,
assembly and concealment); the JS glue shrinks from 18,628 to 13,222 bytes.

## AD-018 : JCT-VC conformance is the regression net for decoder changes

**Context**: the fixture oracles are x265 streams, which exercise few of the
tools a camera or a third-party encoder may use. On the HEVC_v1 suite the base
of this branch matched ffmpeg on 58 of 147 streams and hung on 6.

**Decision**: `tools/jctvc_conformance.py` downloads the suite once, compares
the CLI with ffmpeg and, where they differ, with the suite's own whole-YUV MD5
(status `pass-suite`). `--compare` against an earlier results file lists
regressions. Every decoder fix on this branch was checked with it, and streams
that guard a fix are committed as oracles (`jctvc_*` in
tests/conformance/fixtures). Fixes found this way: tile substreams (entry
points, context initialisation), WPP on one-CTB-wide pictures, entry-point
offsets with emulation prevention, the QP predictor of dependent slice
segments, PCM (alignment, QpY, filter grids, sample depth), the parallel merge
level and merge pruning, constrained intra prediction, and prevTid0Pic.

**Result**: 139 of 147 match ffmpeg; 4 more match the suite's MD5 where ffmpeg
does not (SAODBLK_A/B, VPSSPSPPS_A, TSUNEQBD_A_MAIN10). The remaining 4
(BUMPING_A, NoOutPrior_A/B, RAP_B) differ by design: pictures still waiting
for output at an IRAP with NoOutputOfPriorPicsFlag = 1 (including a CRA after
EOS) are output instead of discarded (AD-010), so every fed frame comes back.

## AD-019 : Review fixes — assembly of damaged input, DPB cap, concealment boundaries

**Context**: two adversarial reviews of this branch found: a heap-use-after-free
when the DPB cap dropped a picture the current RPS still listed; one corrupted
slice turning into two output frames with the same POC and tag (the failed
slice claimed the CTBs it overran, so the next slice of the picture looked like
a new picture), and the orphaned dependent segment arming skip-to-IRAP under
concealment; a repeated slice NAL ending the picture and freezing the default
policy until the next IRAP (the base build decoded such a stream cleanly); a
VPS/SPS/PPS between two slices of one picture splitting it; concealment after
`discontinuity()` using pictures from before the cut; P/B slices with no
reference picture at all decoding mid-grey with no flag; and concealed samples
depending on when the caller drained.

**Decision**:
- DPB cap: the picture dropped to stay at 16 is one the current picture does
  not predict from if possible, and is removed from every RPS set — it becomes
  a missing reference (flagged, substituted under concealment). The RPS is
  *not* additionally bounded by `sps_max_dec_pic_buffering_minus1` (§7.4.8,
  §7.4.7.1): refusing such a slice would drop the picture and freeze; keeping
  it costs one missing reference. ffmpeg does not enforce it either.
- Assembly (AD-013): a segment joins the pending picture when it starts after
  the last segment's start; one that overlaps what an earlier segment decoded
  marks the picture in error. Dropped and counted in `nal_errors`:
  - a NAL whose signature (type, size, first and last 64 payload bytes)
    repeats a segment of the pending picture, or a non-first segment of the
    picture just finished;
  - the first segment of the picture just finished repeated *with the same
    input* (same tag, or the same untagged `feed()` call). Fed again in a
    later call it is another picture: a camera repeating an IDR as a still
    image gets a frame per input;
  - a segment behind the last one carrying the picture's POC, and a first
    slice naming the pending picture's POC (non-IRAP);
  - a picture continuing a CVS whose (CVS, POC) is among the last 32
    pictures started (§8.3.1: POC is unique within a CVS). The history is
    cleared by `flush()`, `discontinuity()` and `reset()`, so a segment
    re-fed after `flush()` (a retried transcode) decodes again;
  - with tagged input (one access unit per tag, as an MP4 sample or RTP
    frame delivers it): a non-first segment that cannot continue the picture
    pending or just finished in the same tagged unit (no picture begins
    mid-unit without its first segment), and an IDR's first segment arriving
    after its other segments in the same unit. Untagged (NaN) feeds are not
    subject to these two rules; callers packing several access units under
    one tag lose the pictures whose first segment was lost.
  An orphan dependent segment arms skip-to-IRAP only under the default
  policy. The finished-picture record is cleared by `flush()`,
  `discontinuity()`, `reset()` and EOS/EOB.
- Parameter sets between the slices of a picture (§7.4.2.4.4; §7.4.3.3) are
  held back (up to 32; more are counted in `nal_errors`) and applied when the
  picture finishes, before the next picture is set up — they take effect from
  the next picture, and the pending picture keeps the sets it started with.
  `reset(false)` applies them; `reset()` drops them with the stored sets.
- A CTB decoded a second time (the slice after a failed one re-covering it)
  has its SAO parameters and deblocking edges reset first.
- Concealment boundaries: see AD-015 (the cut) and AD-016 (reference pictures
  only, empty list entries).

**Measured** (seven QCIF fixtures, single- and multi-slice, 2100 runs per
mutation type, corruption kept inside access units, one tag per access
unit): runs with frames sharing a tag went from 1455/2100 (repeated NAL,
every policy) and up to 224/2100 (reordered NALs, concealment) to 0, except
3 of the 23100 runs under concealment, where a bit error made a slice header
parse as another picture's first slice; those frames are flagged `CORRUPT`. Dropping such a header would also drop the next
picture of a caller packing several access units under one tag.

**Not changed**: a slice that runs past the last CTB without
`end_of_slice_segment_flag`, or whose CABAC reads run past its data, stays an
error under the default policy (dropped, skip-to-IRAP), as AD-016 decided: the
old build output such pictures as clean. Batch `decode()` sorts all pictures by
(CVS, POC) at the end; when NAL units arrive out of decoding order (a slice of
an earlier picture after a later one), streaming output, which cannot wait for
them, can differ in order from batch output.
