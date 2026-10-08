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
