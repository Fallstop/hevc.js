// Brute-force regression guard for the SSE2 (->wasm128) column-parallel small
// inverse transforms (idct4 / idst4 / idct8) added in transform.cpp.
//
// We drive the public perform_transform_inverse() entry point (which uses the
// SIMD small transforms for 4x4 and 8x8) and compare every output byte against
// an INDEPENDENT scalar 2D reference implemented locally here. This locks the
// SIMD path to bit-exact equality with the spec butterfly across many randomized
// full-int16-range coefficient blocks (stressing the Clip3 saturation paths).

#include "decoding/transform.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <vector>
#include <algorithm>

namespace {

inline int clip16(int v) { return std::max(-32768, std::min(32767, v)); }
inline int16_t out(int v, int add, int shift) {
    return static_cast<int16_t>(clip16((v + add) >> shift));
}

// --- scalar reference 1D passes (column-major, mirroring transform.cpp scalar) ---
void ref_idst4(const int16_t* src, int16_t* dst, int shift, int line) {
    int add = 1 << (shift - 1);
    for (int j = 0; j < line; j++) {
        int c0 = src[0*line+j], c1 = src[1*line+j], c2 = src[2*line+j], c3 = src[3*line+j];
        int s0 = 29*c0 + 74*c1 + 84*c2 + 55*c3;
        int s1 = 55*c0 + 74*c1 - 29*c2 - 84*c3;
        int s2 = 74*c0 +  0*c1 - 74*c2 + 74*c3;
        int s3 = 84*c0 - 74*c1 + 55*c2 - 29*c3;
        dst[0*line+j]=out(s0,add,shift); dst[1*line+j]=out(s1,add,shift);
        dst[2*line+j]=out(s2,add,shift); dst[3*line+j]=out(s3,add,shift);
    }
}
void ref_idct4(const int16_t* src, int16_t* dst, int shift, int line) {
    int add = 1 << (shift - 1);
    for (int j = 0; j < line; j++) {
        int E0 = 64*src[0*line+j] + 64*src[2*line+j];
        int E1 = 64*src[0*line+j] - 64*src[2*line+j];
        int O0 = 83*src[1*line+j] + 36*src[3*line+j];
        int O1 = 36*src[1*line+j] - 83*src[3*line+j];
        dst[0*line+j]=out(E0+O0,add,shift); dst[1*line+j]=out(E1+O1,add,shift);
        dst[2*line+j]=out(E1-O1,add,shift); dst[3*line+j]=out(E0-O0,add,shift);
    }
}
void ref_idct8(const int16_t* src, int16_t* dst, int shift, int line) {
    int add = 1 << (shift - 1);
    for (int j = 0; j < line; j++) {
        int EE0 = 64*src[0*line+j] + 64*src[4*line+j];
        int EE1 = 64*src[0*line+j] - 64*src[4*line+j];
        int EO0 = 83*src[2*line+j] + 36*src[6*line+j];
        int EO1 = 36*src[2*line+j] - 83*src[6*line+j];
        int E0=EE0+EO0, E3=EE0-EO0, E1=EE1+EO1, E2=EE1-EO1;
        int O0 = 89*src[1*line+j] + 75*src[3*line+j] + 50*src[5*line+j] + 18*src[7*line+j];
        int O1 = 75*src[1*line+j] - 18*src[3*line+j] - 89*src[5*line+j] - 50*src[7*line+j];
        int O2 = 50*src[1*line+j] - 89*src[3*line+j] + 18*src[5*line+j] + 75*src[7*line+j];
        int O3 = 18*src[1*line+j] - 50*src[3*line+j] + 75*src[5*line+j] - 89*src[7*line+j];
        dst[0*line+j]=out(E0+O0,add,shift); dst[1*line+j]=out(E1+O1,add,shift);
        dst[2*line+j]=out(E2+O2,add,shift); dst[3*line+j]=out(E3+O3,add,shift);
        dst[4*line+j]=out(E3-O3,add,shift); dst[5*line+j]=out(E2-O2,add,shift);
        dst[6*line+j]=out(E1-O1,add,shift); dst[7*line+j]=out(E0-O0,add,shift);
    }
}

enum class Kind { IDCT4, IDST4, IDCT8 };

// Independent scalar 2D inverse transform mirroring inverse_transform_2d().
void ref_2d(Kind kind, int bit_depth, const int16_t* coeff, int16_t* residual) {
    int trSize = (kind == Kind::IDCT8) ? 8 : 4;
    std::vector<int16_t> tmp(trSize*trSize), tmp2(trSize*trSize);
    int shift1 = 7;
    int shift2 = 20 - bit_depth;
    auto pass = [&](const int16_t* in, int16_t* o, int shift) {
        switch (kind) {
            case Kind::IDST4: ref_idst4(in, o, shift, trSize); break;
            case Kind::IDCT4: ref_idct4(in, o, shift, trSize); break;
            case Kind::IDCT8: ref_idct8(in, o, shift, trSize); break;
        }
    };
    pass(coeff, tmp.data(), shift1);
    for (int y = 0; y < trSize; y++)
        for (int x = 0; x < trSize; x++)
            tmp2[y*trSize+x] = tmp[x*trSize+y];
    pass(tmp2.data(), tmp.data(), shift2);
    for (int y = 0; y < trSize; y++)
        for (int x = 0; x < trSize; x++)
            residual[y*trSize+x] = tmp[x*trSize+y];
}

// Inclusive non-zero bounding box of a coefficient block (mirrors perform_dequant).
void bbox(const std::vector<int16_t>& coeff, int trSize, int& lastX, int& lastY) {
    lastX = 0; lastY = 0;
    for (int y = 0; y < trSize; y++)
        for (int x = 0; x < trSize; x++)
            if (coeff[y*trSize+x] != 0) {
                if (x > lastX) lastX = x;
                if (y > lastY) lastY = y;
            }
}

void run_case(Kind kind, int bit_depth, const std::vector<int16_t>& coeff) {
    int trSize = (kind == Kind::IDCT8) ? 8 : 4;
    bool is_intra = (kind == Kind::IDST4); // DST path is luma intra 4x4
    int cIdx = 0;
    int log2 = (kind == Kind::IDCT8) ? 3 : 2;
    std::vector<int16_t> got(trSize*trSize), exp(trSize*trSize), gotBox(trSize*trSize);
    // Full path (lastX/lastY unknown).
    hevc::perform_transform_inverse(log2, cIdx, is_intra, /*transform_skip=*/false,
                                    bit_depth, coeff.data(), got.data());
    ref_2d(kind, bit_depth, coeff.data(), exp.data());
    ASSERT_EQ(got, exp);

    // Non-zero-region skip path: passing the exact bounding box MUST stay bit-exact.
    int lastX, lastY;
    bbox(coeff, trSize, lastX, lastY);
    hevc::perform_transform_inverse(log2, cIdx, is_intra, /*transform_skip=*/false,
                                    bit_depth, coeff.data(), gotBox.data(),
                                    lastX, lastY);
    ASSERT_EQ(gotBox, exp);
}

void brute(Kind kind, int bit_depth, int iters, uint32_t seed) {
    int trSize = (kind == Kind::IDCT8) ? 8 : 4;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> full(-32768, 32767);
    std::uniform_int_distribution<int> small(-8, 8);
    std::vector<int16_t> coeff(trSize*trSize);
    for (int it = 0; it < iters; it++) {
        bool sparse = (it % 3 == 0);
        for (auto& c : coeff) c = static_cast<int16_t>(sparse ? small(rng) : full(rng));
        run_case(kind, bit_depth, coeff);
    }
}

} // namespace

TEST(SmallTransformsSIMD, Idct4MatchesScalar8bit)  { brute(Kind::IDCT4, 8, 4000, 1); }
TEST(SmallTransformsSIMD, Idct4MatchesScalar10bit) { brute(Kind::IDCT4, 10, 4000, 2); }
TEST(SmallTransformsSIMD, Idst4MatchesScalar8bit)  { brute(Kind::IDST4, 8, 4000, 3); }
TEST(SmallTransformsSIMD, Idst4MatchesScalar10bit) { brute(Kind::IDST4, 10, 4000, 4); }
TEST(SmallTransformsSIMD, Idct8MatchesScalar8bit)  { brute(Kind::IDCT8, 8, 4000, 5); }
TEST(SmallTransformsSIMD, Idct8MatchesScalar10bit) { brute(Kind::IDCT8, 10, 4000, 6); }

// Non-zero-region skip: sparse blocks where the bounding box is strictly smaller
// than the transform size (DC-only, single column, single row, partial corner).
// These are the cases the region skip actually shortcuts; run_case() asserts the
// skip path is byte-identical to the full reference.
TEST(SmallTransformsSIMD, NonZeroRegionSkipSparse) {
    std::mt19937 rng(777);
    std::uniform_int_distribution<int> full(-32768, 32767);
    for (Kind k : {Kind::IDCT4, Kind::IDST4, Kind::IDCT8}) {
        int trSize = (k == Kind::IDCT8) ? 8 : 4;
        for (int bd : {8, 10}) {
            // DC-only.
            {
                std::vector<int16_t> c(trSize*trSize, 0);
                c[0] = static_cast<int16_t>(full(rng));
                run_case(k, bd, c);
            }
            // Sweep a rectangular non-zero region [0,bx]x[0,by] of random values.
            for (int by = 0; by < trSize; by++) {
                for (int bx = 0; bx < trSize; bx++) {
                    std::vector<int16_t> c(trSize*trSize, 0);
                    for (int y = 0; y <= by; y++)
                        for (int x = 0; x <= bx; x++)
                            c[y*trSize+x] = static_cast<int16_t>(full(rng));
                    // Ensure the box corners are non-zero so the bbox is exact.
                    c[by*trSize+bx] = c[by*trSize+bx] ? c[by*trSize+bx] : 1;
                    run_case(k, bd, c);
                }
            }
        }
    }
}

// Saturation boundary: all-max and all-min DC-heavy blocks must clip identically.
TEST(SmallTransformsSIMD, SaturationExtremes) {
    for (Kind k : {Kind::IDCT4, Kind::IDST4, Kind::IDCT8}) {
        int trSize = (k == Kind::IDCT8) ? 8 : 4;
        for (int bd : {8, 10}) {
            std::vector<int16_t> mx(trSize*trSize, 32767), mn(trSize*trSize, -32768);
            run_case(k, bd, mx);
            run_case(k, bd, mn);
        }
    }
}
