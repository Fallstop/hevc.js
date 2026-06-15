#include "decoding/interpolation.h"
#include "decoding/coding_tree.h"
#include "decoding/dpb.h"
#include "common/debug.h"

#include <algorithm>
#include <cstring>

namespace hevc {

// ============================================================
// Luma interpolation filter coefficients — Table 8-1
// ============================================================

// §8.5.3.3.3: 8-tap filter, 4 fractional positions (1/4 pel)
// Index 0 = integer (not used in filtering, just copy)
static const int16_t luma_filter[4][8] = {
    {  0,  0,  0, 64,  0,  0,  0,  0 },  // frac=0 (integer)
    { -1,  4,-10, 58, 17, -5,  1,  0 },  // frac=1 (1/4)
    { -1,  4,-11, 40, 40,-11,  4, -1 },  // frac=2 (1/2)
    {  0,  1, -5, 17, 58,-10,  4, -1 },  // frac=3 (3/4)
};

// ============================================================
// Chroma interpolation filter coefficients — Table 8-2
// ============================================================

// §8.5.3.3.3: 4-tap filter, 8 fractional positions (1/8 pel)
static const int16_t chroma_filter[8][4] = {
    {  0, 64,  0,  0 },  // frac=0 (integer)
    { -2, 58, 10, -2 },  // frac=1
    { -4, 54, 16, -2 },  // frac=2
    { -6, 46, 28, -4 },  // frac=3
    { -4, 36, 36, -4 },  // frac=4
    { -4, 28, 46, -6 },  // frac=5
    { -2, 16, 54, -4 },  // frac=6
    { -2, 10, 58, -2 },  // frac=7
};

// ============================================================
// Portable SSE2 interpolation kernels.
// SSE2 intrinsics compile natively on x86-64 (baseline, no flags) AND under
// emscripten -msimd128 (auto-translated to wasm128, integer-identical). One
// source -> both the oracle-verified native build and the shipped WASM.
// ============================================================
// __SSE2__ is set on native x86-64 (baseline) and under emscripten when -msse2 is
// passed (the WASM build adds it; emscripten lowers SSE2 intrinsics to wasm128).
#if defined(__SSE2__)
  #define HEVC_SIMD_INTERP 1
  #include <emmintrin.h>
#endif

// o[x] = (Σ_{k<NTAP} coef[k] * src[x + k*step]) >> shift, for x in [0,width).
// `src` is read as signed int16: picture samples (0..1023) reinterpret losslessly;
// the H-pass intermediate `tmp` is genuinely signed. Per §8.5.3.3.3 every
// interpolation intermediate fits int16, so the saturating pack equals the
// scalar truncating cast — bit-exact with the clamped scalar path.
template<int NTAP>
static inline void simd_filter_row(const int16_t* src, int step,
                                   const int16_t* coef, int shift,
                                   int width, int16_t* o) {
    int x = 0;
#ifdef HEVC_SIMD_INTERP
    // Pairwise-madd kernel. Each tap-pair (2p,2p+1) is packed into an int32 lane
    // [c2p | c2p+1<<16]; interleaving two sample loads gives [s_a,s_b,...] so
    // _mm_madd_epi16 computes c2p*s_a + c2p+1*s_b per output. madd lowers to a single
    // wasm i32x4.dot_i16x8_s — fast on WASM and native alike (NTAP is 4 or 8, even).
    static_assert(NTAP % 2 == 0, "NTAP must be even");
    __m128i cp[NTAP / 2];
    for (int p = 0; p < NTAP / 2; p++)
        cp[p] = _mm_set1_epi32(static_cast<uint16_t>(coef[2 * p]) |
                               (static_cast<int>(coef[2 * p + 1]) << 16));
    const __m128i vsh = _mm_cvtsi32_si128(shift);
    for (; x + 8 <= width; x += 8) {
        __m128i acc_lo = _mm_setzero_si128();  // outputs x..x+3
        __m128i acc_hi = _mm_setzero_si128();  // outputs x+4..x+7
        for (int p = 0; p < NTAP / 2; p++) {
            __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x + (2 * p) * step));
            __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x + (2 * p + 1) * step));
            acc_lo = _mm_add_epi32(acc_lo, _mm_madd_epi16(_mm_unpacklo_epi16(va, vb), cp[p]));
            acc_hi = _mm_add_epi32(acc_hi, _mm_madd_epi16(_mm_unpackhi_epi16(va, vb), cp[p]));
        }
        acc_lo = _mm_sra_epi32(acc_lo, vsh);
        acc_hi = _mm_sra_epi32(acc_hi, vsh);
        // Truncating int32->int16 to match scalar static_cast<int16_t> (WRAPS, not
        // saturates): frac=2 half-pel two-pass V can land just past int16 range.
        // Sign-extending the low 16 bits turns the saturating pack into a truncation.
        acc_lo = _mm_srai_epi32(_mm_slli_epi32(acc_lo, 16), 16);
        acc_hi = _mm_srai_epi32(_mm_slli_epi32(acc_hi, 16), 16);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + x), _mm_packs_epi32(acc_lo, acc_hi));
    }
#endif
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < NTAP; k++) sum += coef[k] * src[x + k * step];
        o[x] = static_cast<int16_t>(sum >> shift);
    }
}

// o[x] = src[x] << shift (integer MV / full-pel copy). Fits int16 by spec.
static inline void simd_copy_row(const uint16_t* src, int shift, int width, int16_t* o) {
    int x = 0;
#ifdef HEVC_SIMD_INTERP
    const __m128i vsh = _mm_cvtsi32_si128(shift);
    for (; x + 8 <= width; x += 8) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + x), _mm_sll_epi16(s, vsh));
    }
#endif
    for (; x < width; x++) o[x] = static_cast<int16_t>(src[x] << shift);
}

// uint8-source variants of the two kernels above: load 8 bytes, zero-extend to
// int16, then run the identical madd / shift / truncate. 8-bit samples (0..255)
// zero-extend to exactly the int16 values the uint16 kernel loads, so these are
// bit-exact with simd_filter_row / simd_copy_row — only the load is half the
// bytes. (step is in samples == bytes for uint8 planes.)
template<int NTAP>
static inline void simd_filter_row_u8(const uint8_t* src, int step,
                                      const int16_t* coef, int shift,
                                      int width, int16_t* o) {
    int x = 0;
#ifdef HEVC_SIMD_INTERP
    static_assert(NTAP % 2 == 0, "NTAP must be even");
    const __m128i zero = _mm_setzero_si128();
    __m128i cp[NTAP / 2];
    for (int p = 0; p < NTAP / 2; p++)
        cp[p] = _mm_set1_epi32(static_cast<uint16_t>(coef[2 * p]) |
                               (static_cast<int>(coef[2 * p + 1]) << 16));
    const __m128i vsh = _mm_cvtsi32_si128(shift);
    for (; x + 8 <= width; x += 8) {
        __m128i acc_lo = _mm_setzero_si128();
        __m128i acc_hi = _mm_setzero_si128();
        for (int p = 0; p < NTAP / 2; p++) {
            __m128i va = _mm_unpacklo_epi8(
                _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + x + (2 * p) * step)), zero);
            __m128i vb = _mm_unpacklo_epi8(
                _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + x + (2 * p + 1) * step)), zero);
            acc_lo = _mm_add_epi32(acc_lo, _mm_madd_epi16(_mm_unpacklo_epi16(va, vb), cp[p]));
            acc_hi = _mm_add_epi32(acc_hi, _mm_madd_epi16(_mm_unpackhi_epi16(va, vb), cp[p]));
        }
        acc_lo = _mm_sra_epi32(acc_lo, vsh);
        acc_hi = _mm_sra_epi32(acc_hi, vsh);
        acc_lo = _mm_srai_epi32(_mm_slli_epi32(acc_lo, 16), 16);
        acc_hi = _mm_srai_epi32(_mm_slli_epi32(acc_hi, 16), 16);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + x), _mm_packs_epi32(acc_lo, acc_hi));
    }
#endif
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < NTAP; k++) sum += coef[k] * src[x + k * step];
        o[x] = static_cast<int16_t>(sum >> shift);
    }
}

static inline void simd_copy_row_u8(const uint8_t* src, int shift, int width, int16_t* o) {
    int x = 0;
#ifdef HEVC_SIMD_INTERP
    const __m128i zero = _mm_setzero_si128();
    const __m128i vsh = _mm_cvtsi32_si128(shift);
    for (; x + 8 <= width; x += 8) {
        __m128i s = _mm_unpacklo_epi8(
            _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + x)), zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + x), _mm_sll_epi16(s, vsh));
    }
#endif
    for (; x < width; x++) o[x] = static_cast<int16_t>(src[x] << shift);
}

// ============================================================
// Luma interpolation — §8.5.3.3.3
// Output in extended precision (not clipped to [0, 2^BitDepth-1])
// ============================================================

template<typename Sample>
static void interpolate_luma_impl(const Picture& refPic,
                              int xInt, int yInt, int xFrac, int yFrac,
                              int nPbW, int nPbH, int bitDepth,
                              int16_t* pred) {
    // §8.5.3.3.3: shift1 = Min(4, BitDepthY - 8), shift2 = 6, shift3 = Max(2, 14 - BitDepthY)
    int shift1 = std::min(4, bitDepth - 8);
    int shift2 = 6;
    int shift3 = std::max(2, 14 - bitDepth);
    int picW = refPic.width[0];
    int picH = refPic.height[0];
    int stride0 = refPic.stride[0];
    const Sample* plane0 = refPic.plane_ptr<Sample>(0);

    // Safe clamped access — used for edge PUs
    auto refClamp = [&](int x, int y) -> int {
        x = std::max(0, std::min(x, picW - 1));
        y = std::max(0, std::min(y, picH - 1));
        return plane0[y * stride0 + x];
    };

    // Check if all reference accesses are within bounds (including filter margin)
    bool interior = (xInt - 3 >= 0) && (yInt - 3 >= 0) &&
                    (xInt + nPbW + 4 <= picW) && (yInt + nPbH + 4 <= picH);

    // Use a macro to avoid duplicating the filter code for interior vs edge
    #define LUMA_INTERP(REF) do { \
        if (xFrac == 0 && yFrac == 0) { \
            for (int y = 0; y < nPbH; y++) \
                for (int x = 0; x < nPbW; x++) \
                    pred[y * nPbW + x] = static_cast<int16_t>(REF(xInt + x, yInt + y) << shift3); \
        } else if (yFrac == 0) { \
            const int16_t* f = luma_filter[xFrac]; \
            for (int y = 0; y < nPbH; y++) \
                for (int x = 0; x < nPbW; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 8; k++) \
                        sum += f[k] * REF(xInt + x + k - 3, yInt + y); \
                    pred[y * nPbW + x] = static_cast<int16_t>(sum >> shift1); \
                } \
        } else if (xFrac == 0) { \
            const int16_t* f = luma_filter[yFrac]; \
            for (int y = 0; y < nPbH; y++) \
                for (int x = 0; x < nPbW; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 8; k++) \
                        sum += f[k] * REF(xInt + x, yInt + y + k - 3); \
                    pred[y * nPbW + x] = static_cast<int16_t>(sum >> shift1); \
                } \
        } else { \
            int tmpH = nPbH + 7; \
            int16_t tmp[64 * 71]; \
            const int16_t* fH = luma_filter[xFrac]; \
            for (int y = 0; y < tmpH; y++) \
                for (int x = 0; x < nPbW; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 8; k++) \
                        sum += fH[k] * REF(xInt + x + k - 3, yInt + y - 3); \
                    tmp[y * nPbW + x] = static_cast<int16_t>(sum >> shift1); \
                } \
            const int16_t* fV = luma_filter[yFrac]; \
            for (int y = 0; y < nPbH; y++) \
                for (int x = 0; x < nPbW; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 8; k++) \
                        sum += fV[k] * tmp[(y + k) * nPbW + x]; \
                    pred[y * nPbW + x] = static_cast<int16_t>(sum >> shift2); \
                } \
        } \
    } while(0)

    if constexpr (sizeof(Sample) == 2) {
    if (interior) {
        // Interior fast path: SSE2 separable filter (bit-exact with the scalar
        // clamped path below). The interior margin (-3 / +4) guarantees the 8-wide
        // loads stay in-bounds. uint16 storage only — the loads reinterpret the
        // plane as int16; the uint8 path uses the scalar branch (SIMD-widen TODO).
        const uint16_t* base = plane0 + yInt * stride0 + xInt;
        if (xFrac == 0 && yFrac == 0) {
            for (int y = 0; y < nPbH; y++)
                simd_copy_row(base + y * stride0, shift3, nPbW, pred + y * nPbW);
        } else if (yFrac == 0) {
            const int16_t* f = luma_filter[xFrac];
            for (int y = 0; y < nPbH; y++)
                simd_filter_row<8>(reinterpret_cast<const int16_t*>(base + y * stride0 - 3),
                                   1, f, shift1, nPbW, pred + y * nPbW);
        } else if (xFrac == 0) {
            const int16_t* f = luma_filter[yFrac];
            for (int y = 0; y < nPbH; y++)
                simd_filter_row<8>(reinterpret_cast<const int16_t*>(base + (y - 3) * stride0),
                                   stride0, f, shift1, nPbW, pred + y * nPbW);
        } else {
            int tmpH = nPbH + 7;
            int16_t tmp[64 * 71];
            const int16_t* fH = luma_filter[xFrac];
            for (int y = 0; y < tmpH; y++)
                simd_filter_row<8>(reinterpret_cast<const int16_t*>(base + (y - 3) * stride0 - 3),
                                   1, fH, shift1, nPbW, tmp + y * nPbW);
            const int16_t* fV = luma_filter[yFrac];
            for (int y = 0; y < nPbH; y++)
                simd_filter_row<8>(tmp + y * nPbW, nPbW, fV, shift2, nPbW, pred + y * nPbW);
        }
    } else {
        LUMA_INTERP(refClamp);
    }
    } else {
        // uint8 storage: SIMD interior (loads bytes, widens to int16 — bit-exact
        // with the uint16 kernel above), scalar clamped path for edge PUs.
        if (interior) {
            const uint8_t* base = plane0 + yInt * stride0 + xInt;
            if (xFrac == 0 && yFrac == 0) {
                for (int y = 0; y < nPbH; y++)
                    simd_copy_row_u8(base + y * stride0, shift3, nPbW, pred + y * nPbW);
            } else if (yFrac == 0) {
                const int16_t* f = luma_filter[xFrac];
                for (int y = 0; y < nPbH; y++)
                    simd_filter_row_u8<8>(base + y * stride0 - 3, 1, f, shift1, nPbW, pred + y * nPbW);
            } else if (xFrac == 0) {
                const int16_t* f = luma_filter[yFrac];
                for (int y = 0; y < nPbH; y++)
                    simd_filter_row_u8<8>(base + (y - 3) * stride0, stride0, f, shift1, nPbW, pred + y * nPbW);
            } else {
                int tmpH = nPbH + 7;
                int16_t tmp[64 * 71];
                const int16_t* fH = luma_filter[xFrac];
                for (int y = 0; y < tmpH; y++)
                    simd_filter_row_u8<8>(base + (y - 3) * stride0 - 3, 1, fH, shift1, nPbW, tmp + y * nPbW);
                // V pass reads the int16 intermediate — reuse the int16 kernel.
                const int16_t* fV = luma_filter[yFrac];
                for (int y = 0; y < nPbH; y++)
                    simd_filter_row<8>(tmp + y * nPbW, nPbW, fV, shift2, nPbW, pred + y * nPbW);
            }
        } else {
            LUMA_INTERP(refClamp);
        }
    }

    #undef LUMA_INTERP
}

// Dispatch on reference-plane storage width.
static void interpolate_luma(const Picture& refPic,
                             int xInt, int yInt, int xFrac, int yFrac,
                             int nPbW, int nPbH, int bitDepth, int16_t* pred) {
    if (refPic.bytes_per_sample == 1)
        interpolate_luma_impl<uint8_t>(refPic, xInt, yInt, xFrac, yFrac, nPbW, nPbH, bitDepth, pred);
    else
        interpolate_luma_impl<uint16_t>(refPic, xInt, yInt, xFrac, yFrac, nPbW, nPbH, bitDepth, pred);
}

// ============================================================
// Chroma interpolation — §8.5.3.3.3 (chroma part)
// ============================================================

template<typename Sample>
static void interpolate_chroma_impl(const Picture& refPic, int cIdx,
                                int xInt, int yInt, int xFrac, int yFrac,
                                int nPbWC, int nPbHC, int bitDepth,
                                int16_t* pred) {
    int shift1 = std::min(4, bitDepth - 8);
    int shift2 = 6;
    int shift3 = std::max(2, 14 - bitDepth);
    int picW = refPic.width[cIdx];
    int picH = refPic.height[cIdx];
    int strideC = refPic.stride[cIdx];
    const Sample* planeC = refPic.plane_ptr<Sample>(cIdx);

    auto refClamp = [&](int x, int y) -> int {
        x = std::max(0, std::min(x, picW - 1));
        y = std::max(0, std::min(y, picH - 1));
        return planeC[y * strideC + x];
    };

    // Chroma filter margin is 1 (4-tap: positions -1..+2)
    bool interior = (xInt - 1 >= 0) && (yInt - 1 >= 0) &&
                    (xInt + nPbWC + 2 <= picW) && (yInt + nPbHC + 2 <= picH);

    #define CHROMA_INTERP(REF) do { \
        if (xFrac == 0 && yFrac == 0) { \
            for (int y = 0; y < nPbHC; y++) \
                for (int x = 0; x < nPbWC; x++) \
                    pred[y * nPbWC + x] = static_cast<int16_t>(REF(xInt + x, yInt + y) << shift3); \
        } else if (yFrac == 0) { \
            const int16_t* f = chroma_filter[xFrac]; \
            for (int y = 0; y < nPbHC; y++) \
                for (int x = 0; x < nPbWC; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 4; k++) \
                        sum += f[k] * REF(xInt + x + k - 1, yInt + y); \
                    pred[y * nPbWC + x] = static_cast<int16_t>(sum >> shift1); \
                } \
        } else if (xFrac == 0) { \
            const int16_t* f = chroma_filter[yFrac]; \
            for (int y = 0; y < nPbHC; y++) \
                for (int x = 0; x < nPbWC; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 4; k++) \
                        sum += f[k] * REF(xInt + x, yInt + y + k - 1); \
                    pred[y * nPbWC + x] = static_cast<int16_t>(sum >> shift1); \
                } \
        } else { \
            int tmpH = nPbHC + 3; \
            int16_t tmp[32 * 35]; \
            const int16_t* fH = chroma_filter[xFrac]; \
            for (int y = 0; y < tmpH; y++) \
                for (int x = 0; x < nPbWC; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 4; k++) \
                        sum += fH[k] * REF(xInt + x + k - 1, yInt + y - 1); \
                    tmp[y * nPbWC + x] = static_cast<int16_t>(sum >> shift1); \
                } \
            const int16_t* fV = chroma_filter[yFrac]; \
            for (int y = 0; y < nPbHC; y++) \
                for (int x = 0; x < nPbWC; x++) { \
                    int sum = 0; \
                    for (int k = 0; k < 4; k++) \
                        sum += fV[k] * tmp[(y + k) * nPbWC + x]; \
                    pred[y * nPbWC + x] = static_cast<int16_t>(sum >> shift2); \
                } \
        } \
    } while(0)

    if constexpr (sizeof(Sample) == 2) {
    if (interior) {
        // Interior fast path: SSE2 separable 4-tap filter (bit-exact). Chroma margin
        // is only -1 / +2, but the 8-wide load needs +6 of headroom past the last
        // output column; simd_filter_row only vectorizes blocks of 8 and falls back
        // to scalar for the remainder, so for widths < 8 (common in chroma) it runs
        // scalar. For width >= 8, the wide load reaches at most base + nPbWC+2 which
        // the interior test (xInt+nPbWC+2 <= picW) keeps in-bounds; the extra lanes
        // read are within the same valid row. uint16 storage only (reinterpret).
        const uint16_t* base = planeC + yInt * strideC + xInt;
        if (xFrac == 0 && yFrac == 0) {
            for (int y = 0; y < nPbHC; y++)
                simd_copy_row(base + y * strideC, shift3, nPbWC, pred + y * nPbWC);
        } else if (yFrac == 0) {
            const int16_t* f = chroma_filter[xFrac];
            for (int y = 0; y < nPbHC; y++)
                simd_filter_row<4>(reinterpret_cast<const int16_t*>(base + y * strideC - 1),
                                   1, f, shift1, nPbWC, pred + y * nPbWC);
        } else if (xFrac == 0) {
            const int16_t* f = chroma_filter[yFrac];
            for (int y = 0; y < nPbHC; y++)
                simd_filter_row<4>(reinterpret_cast<const int16_t*>(base + (y - 1) * strideC),
                                   strideC, f, shift1, nPbWC, pred + y * nPbWC);
        } else {
            int tmpH = nPbHC + 3;
            int16_t tmp[32 * 35];
            const int16_t* fH = chroma_filter[xFrac];
            for (int y = 0; y < tmpH; y++)
                simd_filter_row<4>(reinterpret_cast<const int16_t*>(base + (y - 1) * strideC - 1),
                                   1, fH, shift1, nPbWC, tmp + y * nPbWC);
            const int16_t* fV = chroma_filter[yFrac];
            for (int y = 0; y < nPbHC; y++)
                simd_filter_row<4>(tmp + y * nPbWC, nPbWC, fV, shift2, nPbWC, pred + y * nPbWC);
        }
    } else {
        CHROMA_INTERP(refClamp);
    }
    } else {
        // uint8 storage: SIMD interior (loads bytes, widens to int16 — bit-exact
        // with the uint16 kernel above), scalar clamped path for edge PUs.
        if (interior) {
            const uint8_t* base = planeC + yInt * strideC + xInt;
            if (xFrac == 0 && yFrac == 0) {
                for (int y = 0; y < nPbHC; y++)
                    simd_copy_row_u8(base + y * strideC, shift3, nPbWC, pred + y * nPbWC);
            } else if (yFrac == 0) {
                const int16_t* f = chroma_filter[xFrac];
                for (int y = 0; y < nPbHC; y++)
                    simd_filter_row_u8<4>(base + y * strideC - 1, 1, f, shift1, nPbWC, pred + y * nPbWC);
            } else if (xFrac == 0) {
                const int16_t* f = chroma_filter[yFrac];
                for (int y = 0; y < nPbHC; y++)
                    simd_filter_row_u8<4>(base + (y - 1) * strideC, strideC, f, shift1, nPbWC, pred + y * nPbWC);
            } else {
                int tmpH = nPbHC + 3;
                int16_t tmp[32 * 35];
                const int16_t* fH = chroma_filter[xFrac];
                for (int y = 0; y < tmpH; y++)
                    simd_filter_row_u8<4>(base + (y - 1) * strideC - 1, 1, fH, shift1, nPbWC, tmp + y * nPbWC);
                const int16_t* fV = chroma_filter[yFrac];
                for (int y = 0; y < nPbHC; y++)
                    simd_filter_row<4>(tmp + y * nPbWC, nPbWC, fV, shift2, nPbWC, pred + y * nPbWC);
            }
        } else {
            CHROMA_INTERP(refClamp);
        }
    }
    #undef CHROMA_INTERP
}

// Dispatch on reference-plane storage width.
static void interpolate_chroma(const Picture& refPic, int cIdx,
                               int xInt, int yInt, int xFrac, int yFrac,
                               int nPbWC, int nPbHC, int bitDepth, int16_t* pred) {
    if (refPic.bytes_per_sample == 1)
        interpolate_chroma_impl<uint8_t>(refPic, cIdx, xInt, yInt, xFrac, yFrac, nPbWC, nPbHC, bitDepth, pred);
    else
        interpolate_chroma_impl<uint16_t>(refPic, cIdx, xInt, yInt, xFrac, yFrac, nPbWC, nPbHC, bitDepth, pred);
}

// ============================================================
// §8.5.3.3.4.2 — Default weighted sample prediction
// ============================================================

#ifdef HEVC_SIMD_INTERP
// out[i] = Clip3(0, maxVal, (a[i] + add) >> sh). 8 int16 lanes/iter, computed in
// int32 to match the scalar (arithmetic >> on a signed int; sra_epi32 is arithmetic
// on native and wasm alike), then narrowed and clamped — bit-exact.
static inline void simd_shift_clip(const int16_t* a, int add, int sh,
                                   int n, int maxVal, int16_t* out) {
    const __m128i vadd = _mm_set1_epi32(add);
    const __m128i vsh = _mm_cvtsi32_si128(sh);
    const __m128i vmax = _mm_set1_epi16(static_cast<int16_t>(maxVal));
    const __m128i zero = _mm_setzero_si128();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i lo = _mm_sra_epi32(_mm_add_epi32(_mm_srai_epi32(_mm_unpacklo_epi16(p, p), 16), vadd), vsh);
        __m128i hi = _mm_sra_epi32(_mm_add_epi32(_mm_srai_epi32(_mm_unpackhi_epi16(p, p), 16), vadd), vsh);
        __m128i v = _mm_max_epi16(_mm_min_epi16(_mm_packs_epi32(lo, hi), vmax), zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), v);
    }
    for (; i < n; i++)
        out[i] = static_cast<int16_t>(Clip3(0, maxVal, (a[i] + add) >> sh));
}
// out[i] = Clip3(0, maxVal, (a[i] + b[i] + add) >> sh). Bi-pred needs the int32
// intermediate: two ~14-bit signed addends can exceed int16.
static inline void simd_avg_clip(const int16_t* a, const int16_t* b, int add, int sh,
                                 int n, int maxVal, int16_t* out) {
    const __m128i vadd = _mm_set1_epi32(add);
    const __m128i vsh = _mm_cvtsi32_si128(sh);
    const __m128i vmax = _mm_set1_epi16(static_cast<int16_t>(maxVal));
    const __m128i zero = _mm_setzero_si128();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i pa = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i pb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i alo = _mm_srai_epi32(_mm_unpacklo_epi16(pa, pa), 16);
        __m128i ahi = _mm_srai_epi32(_mm_unpackhi_epi16(pa, pa), 16);
        __m128i blo = _mm_srai_epi32(_mm_unpacklo_epi16(pb, pb), 16);
        __m128i bhi = _mm_srai_epi32(_mm_unpackhi_epi16(pb, pb), 16);
        __m128i lo = _mm_sra_epi32(_mm_add_epi32(_mm_add_epi32(alo, blo), vadd), vsh);
        __m128i hi = _mm_sra_epi32(_mm_add_epi32(_mm_add_epi32(ahi, bhi), vadd), vsh);
        __m128i v = _mm_max_epi16(_mm_min_epi16(_mm_packs_epi32(lo, hi), vmax), zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), v);
    }
    for (; i < n; i++)
        out[i] = static_cast<int16_t>(Clip3(0, maxVal, (a[i] + b[i] + add) >> sh));
}
#endif  // HEVC_SIMD_INTERP

static void weighted_pred_default(int16_t* predL0, int16_t* predL1,
                                   bool flagL0, bool flagL1,
                                   int nSamples, int bitDepth,
                                   int16_t* output) {
    // §8.5.3.3.4.2
    int shift1 = std::max(2, 14 - bitDepth);
    int offset1 = 1 << (shift1 - 1);
    int shift2 = std::max(3, 15 - bitDepth);
    int offset2 = 1 << (shift2 - 1);
    int maxVal = (1 << bitDepth) - 1;

#ifdef HEVC_SIMD_INTERP
    if (flagL0 && !flagL1)        simd_shift_clip(predL0, offset1, shift1, nSamples, maxVal, output);
    else if (!flagL0 && flagL1)   simd_shift_clip(predL1, offset1, shift1, nSamples, maxVal, output);
    else                          simd_avg_clip(predL0, predL1, offset2, shift2, nSamples, maxVal, output);
#else
    if (flagL0 && !flagL1) {
        // §8.5.3.3.4.2 eq 8-262: uni-pred L0
        for (int i = 0; i < nSamples; i++)
            output[i] = static_cast<int16_t>(Clip3(0, maxVal, (predL0[i] + offset1) >> shift1));
    } else if (!flagL0 && flagL1) {
        // eq 8-263: uni-pred L1
        for (int i = 0; i < nSamples; i++)
            output[i] = static_cast<int16_t>(Clip3(0, maxVal, (predL1[i] + offset1) >> shift1));
    } else {
        // eq 8-264: bi-pred
        for (int i = 0; i < nSamples; i++)
            output[i] = static_cast<int16_t>(Clip3(0, maxVal,
                (predL0[i] + predL1[i] + offset2) >> shift2));
    }
#endif
}

// ============================================================
// §8.5.3.3.4.3 — Explicit weighted sample prediction
// ============================================================

static void weighted_pred_explicit(int16_t* predL0, int16_t* predL1,
                                    bool flagL0, bool flagL1,
                                    int refIdxL0, int refIdxL1,
                                    int cIdx, int nSamples, int bitDepth,
                                    const PredWeightTable& pwt,
                                    int16_t* output) {
    // §8.5.3.3.4.3
    int shift1 = std::max(2, 14 - bitDepth);
    int maxVal = (1 << bitDepth) - 1;

    int log2Wd, w0, w1, o0, o1;

    if (cIdx == 0) {
        // Luma — eq 8-265..8-269
        log2Wd = static_cast<int>(pwt.luma_log2_weight_denom) + shift1;
        w0 = pwt.l0[refIdxL0 >= 0 ? refIdxL0 : 0].luma_weight;
        w1 = pwt.l1[refIdxL1 >= 0 ? refIdxL1 : 0].luma_weight;
        // WpOffsetBdShiftY = BitDepthY - 8
        int wpShiftY = bitDepth - 8;
        o0 = pwt.l0[refIdxL0 >= 0 ? refIdxL0 : 0].luma_offset << wpShiftY;
        o1 = pwt.l1[refIdxL1 >= 0 ? refIdxL1 : 0].luma_offset << wpShiftY;
    } else {
        // Chroma — eq 8-270..8-274
        int chromaLog2WeightDenom = static_cast<int>(pwt.luma_log2_weight_denom) +
                                    pwt.delta_chroma_log2_weight_denom;
        log2Wd = chromaLog2WeightDenom + shift1;
        int ci = cIdx - 1;  // 0=Cb, 1=Cr
        w0 = pwt.l0[refIdxL0 >= 0 ? refIdxL0 : 0].chroma_weight[ci];
        w1 = pwt.l1[refIdxL1 >= 0 ? refIdxL1 : 0].chroma_weight[ci];
        // WpOffsetBdShiftC = BitDepthC - 8
        int wpShiftC = bitDepth - 8;
        o0 = pwt.l0[refIdxL0 >= 0 ? refIdxL0 : 0].chroma_offset[ci] << wpShiftC;
        o1 = pwt.l1[refIdxL1 >= 0 ? refIdxL1 : 0].chroma_offset[ci] << wpShiftC;
    }

    if (flagL0 && !flagL1) {
        // eq 8-275: uni-pred L0
        int round = 1 << (log2Wd - 1);
        for (int i = 0; i < nSamples; i++)
            output[i] = static_cast<int16_t>(Clip3(0, maxVal,
                ((predL0[i] * w0 + round) >> log2Wd) + o0));
    } else if (!flagL0 && flagL1) {
        // eq 8-276: uni-pred L1
        int round = 1 << (log2Wd - 1);
        for (int i = 0; i < nSamples; i++)
            output[i] = static_cast<int16_t>(Clip3(0, maxVal,
                ((predL1[i] * w1 + round) >> log2Wd) + o1));
    } else {
        // eq 8-277: bi-pred
        for (int i = 0; i < nSamples; i++)
            output[i] = static_cast<int16_t>(Clip3(0, maxVal,
                (predL0[i] * w0 + predL1[i] * w1 +
                 ((o0 + o1 + 1) << log2Wd)) >> (log2Wd + 1)));
    }
}

// ============================================================
// §8.5.3.3 — Top-level inter prediction for one PU
// ============================================================

void perform_inter_prediction(DecodingContext& ctx,
                               int xPb, int yPb, int nPbW, int nPbH,
                               int cIdx,
                               const MV& mvL0, const MV& mvL1,
                               int refIdxL0, int refIdxL1,
                               bool predFlagL0, bool predFlagL1,
                               int16_t* pred_samples) {
    auto& sps = *ctx.sps;
    int bitDepth = (cIdx == 0) ? sps.BitDepthY : sps.BitDepthC;

    // Component dimensions and MV conversion
    int subW = (cIdx > 0) ? sps.SubWidthC : 1;
    int subH = (cIdx > 0) ? sps.SubHeightC : 1;
    int compW = nPbW / subW;
    int compH = nPbH / subH;
    int nSamples = compW * compH;

    int16_t predL0_buf[64 * 64];  // max PU 64x64
    int16_t predL1_buf[64 * 64];
    int16_t* predL0 = predL0_buf;
    int16_t* predL1 = predL1_buf;

    // L0 prediction
    if (predFlagL0 && refIdxL0 >= 0) {
        Picture* refPic = ctx.dpb->ref_pic_list0(refIdxL0);
        if (refPic) {
            if (cIdx == 0) {
                // Luma: MV in 1/4 pel
                int xInt = xPb + (mvL0.x >> 2);
                int yInt = yPb + (mvL0.y >> 2);
                int xFrac = mvL0.x & 3;
                int yFrac = mvL0.y & 3;
                interpolate_luma(*refPic, xInt, yInt, xFrac, yFrac,
                                  compW, compH, bitDepth, predL0);
            } else {
                // §8.5.3.3.2: chroma MV derivation from luma MV
                // mvC = (mvL * SubWidth/Height + 2) >> 2... actually:
                // xFracC and yFracC in 1/8 pel
                int mvCx = mvL0.x;
                int mvCy = mvL0.y;
                // §8.5.3.3: chroma MV = luma MV for 4:2:0, but at 1/8 pel precision
                // xIntC = (xPb/SubWidthC) + (mvCx >> (1 + cShiftX))
                // Wait, for 4:2:0: the chroma MV is just luma MV / 2 in full units,
                // and the fractional part is at 1/8 pel
                int xPbC = xPb / subW;
                int yPbC = yPb / subH;
                // Chroma MV derivation: spec §8.5.3.3.2
                // For 4:2:0: mvC_x = mvL_x, mvC_y = mvL_y (same quarter-pel values)
                // But chroma positions: xIntC = xPbC + (mvCx >> 3), xFracC = mvCx & 7
                // because for 4:2:0, the luma MV at 1/4 pel maps to chroma at 1/8 pel
                // (2x downsampling means 1/4 luma pel = 1/8 chroma pel)
                int xInt = xPbC + (mvCx >> 3);
                int yInt = yPbC + (mvCy >> 3);
                int xFrac = mvCx & 7;
                int yFrac = mvCy & 7;
                interpolate_chroma(*refPic, cIdx, xInt, yInt, xFrac, yFrac,
                                    compW, compH, bitDepth, predL0);
            }
        }
    }

    // L1 prediction
    if (predFlagL1 && refIdxL1 >= 0) {
        Picture* refPic = ctx.dpb->ref_pic_list1(refIdxL1);
        if (refPic) {
            if (cIdx == 0) {
                int xInt = xPb + (mvL1.x >> 2);
                int yInt = yPb + (mvL1.y >> 2);
                int xFrac = mvL1.x & 3;
                int yFrac = mvL1.y & 3;
                interpolate_luma(*refPic, xInt, yInt, xFrac, yFrac,
                                  compW, compH, bitDepth, predL1);
            } else {
                int xPbC = xPb / subW;
                int yPbC = yPb / subH;
                int xInt = xPbC + (mvL1.x >> 3);
                int yInt = yPbC + (mvL1.y >> 3);
                int xFrac = mvL1.x & 7;
                int yFrac = mvL1.y & 7;
                interpolate_chroma(*refPic, cIdx, xInt, yInt, xFrac, yFrac,
                                    compW, compH, bitDepth, predL1);
            }
        }
    }

    // §8.5.3.3.4.1: Determine weightedPredFlag
    bool weightedPredFlag = false;
    if (ctx.sh->slice_type == SliceType::P)
        weightedPredFlag = ctx.pps->weighted_pred_flag;
    else if (ctx.sh->slice_type == SliceType::B)
        weightedPredFlag = ctx.pps->weighted_bipred_flag;

    if (weightedPredFlag) {
        // §8.5.3.3.4.3: Explicit weighted sample prediction
        weighted_pred_explicit(predL0, predL1,
                               predFlagL0, predFlagL1,
                               refIdxL0, refIdxL1,
                               cIdx, nSamples, bitDepth,
                               ctx.sh->pred_weight_table, pred_samples);
    } else {
        // §8.5.3.3.4.2: Default weighted sample prediction
        weighted_pred_default(predL0, predL1,
                               predFlagL0, predFlagL1,
                               nSamples, bitDepth, pred_samples);
    }
}

} // namespace hevc
