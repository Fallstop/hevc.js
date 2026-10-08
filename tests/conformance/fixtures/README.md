# Test Fixtures

Bitstreams HEVC de test generes avec x265 4.1. Chaque bitstream a un hash MD5
de reference calcule sur le YUV decode par ffmpeg (libavcodec).

## Bitstreams

| Fichier | Taille | Resolution | Frames | Type | Deblock | SAO | Phase | MD5 (YUV decode) |
|---------|--------|------------|--------|------|---------|-----|-------|------------------|
| `i_64x64_qp22.265` | 511B | 64x64 | 1 | I-only | off | off | 4 | `48c64cd2de381113913149e92065d66c` |
| `i_64x64_deblock.265` | 510B | 64x64 | 1 | I-only | **on** | off | 6 | `576ef497ceedffa01709ddfaa13c76c3` |
| `i_64x64_sao.265` | 515B | 64x64 | 1 | I-only | off | **on** | 6 | `eacb4952591daa891e8dc5344c6c34ee` |
| `i_64x64_full.265` | 514B | 64x64 | 1 | I-only | **on** | **on** | 6 | `dcb7ae02b4514b6e7fd801ae691d901a` |
| `p_qcif_10f.265` | 174K | 176x144 | 10 | I+P | off | off | 5 | `921650ab84b8e366a1aa4e16f2527e46` |
| `b_qcif_10f.265` | 171K | 176x144 | 10 | I+P+B | off | off | 5 | `dffe2712c0561c75ee40256d59299826` |
| `full_qcif_10f.265` | 171K | 176x144 | 10 | I+P+B | **on** | **on** | 6 | `a4984ee61d18ac9619808c5338132eaa` |

## Toy bitstreams (debug step-by-step)

| Fichier | Taille | Resolution | Frames | QP | Phase | MD5 (YUV decode) |
|---------|--------|------------|--------|----|-------|------------------|
| `toy_qp30.265` | 187B | 64x64 | 1 | 30 | 4 | `9f59878470d904dc2e162d3d191611af` |
| `toy_qp45.265` | 141B | 64x64 | 1 | 45 | 4 | `52ca5108bd4354383a9d29d96cf64b47` |
| `toy_qp10.265` | 274B | 64x64 | 1 | 10 | 4 | `ff469ce7e872152490b9b39b0d05dd7b` |

Encodage : `x265 --preset ultrafast --keyint 1 --no-deblock --no-sao --no-wpp --no-info --qp <QP>`

Ces bitstreams ont 1 seul CTU (64x64), ideal pour debugger CABAC et intra prediction etape par etape.

## Real-world bitstreams (Big Buck Bunny)

| Fichier | Taille | Resolution | Frames | FPS | Type | Phase | MD5 (YUV decode) |
|---------|--------|------------|--------|-----|------|-------|------------------|
| `bbb1080_50f.265` | 783K | 1920x1080 | 50 | 25 | I+P+B (full) | 6+ | `a7c12e24701e3636ce58246033bff6ed` |
| `bbb4k_25f.265` | 4.4M | 3840x2160 | 25 | 25 | I+P+B (full) | 6+ | `69f2ba71153ec5f3c1573fed17631ed4` |

Source : Big Buck Bunny, encode HEVC Main profile, extrait sans re-encodage depuis hevc-gpu demo streams.

## Main 10 bitstreams (10-bit 4:2:0)

| Fichier | Taille | Resolution | Frames | Type | Deblock | SAO | Phase | MD5 (YUV decode) |
|---------|--------|------------|--------|------|---------|-----|-------|------------------|
| `i_64x64_10bit.265` | 2.4K | 64x64 | 1 | I-only | off | off | 7 | `230cf887beecf0dc1722a823f998de47` |
| `full_qcif_10f_10bit.265` | 15K | 176x144 | 10 | I+P+B | **on** | **on** | 7 | `19efbf36509c6ecb86e42a7f709998b5` |

Encodage : `x265 --input-depth 10 --output-depth 10 --profile main10 --preset medium/ultrafast --no-wpp --no-info --qp 22`

Reference : `ffmpeg -y -i fixture.265 -pix_fmt yuv420p10le fixture_ref.yuv`

## Extended chroma formats (Main 4:2:2 / 4:4:4, Range Extensions)

QCIF I+P clips that exercise the non-4:2:0 chroma paths. Each is byte-exact
against the ffmpeg/libx265 reference at the matching `-pix_fmt`.

| File | Resolution | Frames | Format | Notes | MD5 (YUV decode) |
|------|------------|--------|--------|-------|------------------|
| `i_p_qcif_422_4f.265` | 176x144 | 4 | 4:2:2 8-bit | deblock + SAO | `ca06427d6ae6af68127f3812b75aa9b5` |
| `i_p_qcif_422_10b_4f.265` | 176x144 | 4 | 4:2:2 10-bit | uint16 storage | `7df5ccbfcca98ff8adce6b7321420f41` |
| `i_p_qcif_422_hq_4f.265` | 176x144 | 4 | 4:2:2 8-bit | qp=42 — §8.6.1 chroma-QP clamp | `52162b65507ff80be5b3aa6819071cf6` |
| `i_p_qcif_444_4f.265` | 176x144 | 4 | 4:4:4 8-bit | SAO; 240 intra-NxN CUs | `8a3d86c4a2452f4aa7f3668f6f710679` |
| `i_p_qcif_444_10b_4f.265` | 176x144 | 4 | 4:4:4 10-bit | uint16 storage | `5192c41e701ab377e96e9de2ee3133f4` |
| `i_p_qcif_444_hq_4f.265` | 176x144 | 4 | 4:4:4 8-bit | qp=6 — dense per-leaf chroma residual | `fe6acf6ed144c98e643d7485debe9c62` |

The 4:4:4 clips are intentionally intra-NxN-heavy: 4:4:4 signals
`intra_chroma_pred_mode` per luma PU (four per NxN CU) and reconstructs a
co-sited chroma transform block at every transform leaf, so NxN content
guards both 4:4:4 CABAC parse paths.

Reference (4:4:4): `ffmpeg -y -i fixture.265 -pix_fmt yuv444p[10le] fixture_ref.yuv`

## Parametres d'encodage communs

```
x265 --preset medium/slow --qp 22 --no-wpp --no-info
```

- Chroma : 4:2:0
- Bit depth : 8-bit
- Profile : Main
- Pas de WPP (single-thread CABAC)
- Pas de SEI info (bitstream plus propre)

## Comment regenerer les references

```bash
# Decoder avec ffmpeg
ffmpeg -y -i fixture.265 -pix_fmt yuv420p fixture_ref.yuv

# Calculer le MD5
md5 -q fixture_ref.yuv  # macOS
md5sum fixture_ref.yuv   # Linux
```

## Comment utiliser dans les tests

```cpp
// Le test oracle decode le bitstream, compare le MD5 du YUV output
// avec le MD5 de reference ci-dessus
TEST(Oracle, I_64x64_QP22) {
    auto output = decode_file("tests/conformance/fixtures/i_64x64_qp22.265");
    EXPECT_EQ(md5(output), "48c64cd2de381113913149e92065d66c");
}
```

## Open-GOP RASL bitstreams (ported from upstream)

| Fichier | Taille | Resolution | Coded | Output | Type | MD5 (YUV decode) |
|---------|--------|------------|-------|--------|------|------------------|
| `opengop_qcif_12f.265` | 4.9K | 176x144 | 12 | 12 | IDR + 2x(CRA + 3 RASL) + trailing | `06e53846ff51cf35cadc97a080147e82` |
| `opengop_qcif_cra_first.265` | 1.9K | 176x144 | 7 | **4** | CRA + 3 RASL + 3 trailing | `9c249fb00174f2f6e6fbb3457c10679b` |

The CRA of `opengop_qcif_12f.265` is mid-stream (NoRaslOutputFlag = 0): its RASL
pictures are decodable and output. `opengop_qcif_cra_first.265` is the same stream cut
at that CRA, which then opens the bitstream (NoRaslOutputFlag = 1): its three RASL
pictures must not be output (§8.1). Generated upstream by `tools/gen_open_gop_fixtures.sh`.

## JCT-VC conformance streams (ITU-T H.265.1)

Unmodified bitstreams from the JCT-VC HEVC_v1 conformance suite, except
`jctvc_MAXBINS_B_TI_5_2f.265` and `jctvc_DSLICE_A_HHI_5_1f.265`: their first
two / first access units, cut with
`ffmpeg -i <stream> -c copy -frames:v <n> -f hevc`. Each MD5 is the
ffmpeg decode, which matches the suite's reference YUV where one is shipped.

| Fichier | Taille | Resolution | Frames | Guards | MD5 (YUV decode) |
|---------|--------|------------|--------|--------|------------------|
| `jctvc_ipcm_C_NEC_3.265` | 5.7K | 416x240 | 1 | PCM alignment, PCM QpY, PCM edges with `pcm_loop_filter_disabled_flag` | `c3e74c399b73a5ab2dbd20523f583464` |
| `jctvc_ipcm_D_NEC_3.265` | 5.7K | 416x240 | 1 | PCM with `cu_transquant_bypass` | `aa64a16240064bc2a90fadf979a62a7b` |
| `jctvc_PMERGE_B_TI_3.265` | 16K | 416x240 | 8 | parallel merge level, 8x4 bi-pred restriction on the coded PU size | `7d0e57ebdbbc6633c972083a17d97550` |
| `jctvc_CIP_A_Panasonic_3.265` | 12K | 416x240 | 2 | constrained intra prediction | `4cbf601ba98d63f642defab5eaa12c8d` |
| `jctvc_POC_A_Bossen_3.265` | 83K | 416x240 | 5 | prevTid0Pic excludes TRAIL_N at TemporalId 0 | `faf33bec64d3e58c426ed2772903a0bf` |
| `jctvc_MAXBINS_B_TI_5_2f.265` | 220K | 416x240 | 2 | merge pruning ignores the unused list after the 8x4 restriction | `7522b8154651a696e053057f440cd11f` |
| `jctvc_DSLICE_A_HHI_5_1f.265` | 36K | 1920x1080 | 1 | WPP with dependent slice segments; slice grouping after a failed segment (unit tests) | `7a7bedeed541f7f5185d33139b378fb1` |

## Robustness bitstreams

Non-conforming on purpose; used by the unit tests, not by the MD5 oracles.

| File | Size | Resolution | Frames | What it breaks |
|------|------|------------|--------|----------------|
| `rps17_qcif_40f.265` | 27K | 176x144 | 40 | `x265 --ref 16 --bframes 0 --allow-non-conformance`, then from picture 16 on each P slice's inline RPS names one more picture (POC n-16): 16 references plus the current picture exceed the 16-picture DPB |
