// Unit tests for explicit (non-default) scaling-list parsing.
//
// Spec §7.3.4 transmits scaling_list_data() coefficients in up-right diagonal
// SCAN order, indexed by scan position i. The dequant read in
// transform.cpp and ScalingListData::set_defaults() both use RASTER layout
// (y*stride + x). ScalingListData::parse() must therefore map each parsed
// scan-position coefficient back to its (xC,yC) raster cell via the inverse
// diagonal scan so the stored matrix is raster — otherwise explicit
// non-default matrices are dequantized scrambled.
//
// These tests feed a known non-default list in scan order and assert the
// stored raster matrix.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "bitstream/bitstream_reader.h"
#include "syntax/sps.h"
#include "decoding/cabac_tables.h"

using namespace hevc;

namespace {

// Minimal MSB-first bit writer to synthesize a scaling_list_data() RBSP for
// the parser. Mirrors the exp-Golomb mapping used by BitstreamReader.
class BitWriter {
public:
    void put_bit(int b) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (b & 1));
        if (++nbits_ == 8) {
            bytes_.push_back(cur_);
            cur_ = 0;
            nbits_ = 0;
        }
    }
    void put_flag(bool f) { put_bit(f ? 1 : 0); }

    // ue(v) — Exp-Golomb unsigned.
    void put_ue(uint32_t v) {
        uint32_t code = v + 1;
        int len = 0;
        while ((1u << (len + 1)) <= code) len++;  // number of leading zeros
        for (int i = 0; i < len; i++) put_bit(0);
        for (int i = len; i >= 0; i--) put_bit((code >> i) & 1);
    }

    // se(v) — Exp-Golomb signed. 0->0, +k->2k-1, -k->2k.
    void put_se(int32_t v) {
        uint32_t code = (v > 0) ? static_cast<uint32_t>(2 * v - 1)
                                : static_cast<uint32_t>(-2 * v);
        put_ue(code);
    }

    std::vector<uint8_t> finish() {
        if (nbits_ > 0) {
            cur_ = static_cast<uint8_t>(cur_ << (8 - nbits_));
            bytes_.push_back(cur_);
            cur_ = 0;
            nbits_ = 0;
        }
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0;
    int nbits_ = 0;
};

// Emit one explicit coefficient list whose value at scan position i is base+i.
// Returns the delta-coef stream embedded directly. nextCoef starts at 8 in the
// parser, so the first delta is (base-8) and each subsequent delta is +1.
void put_ramp_list(BitWriter& w, int base, int coefNum) {
    int prev = 8;  // parser's initial nextCoef
    for (int i = 0; i < coefNum; i++) {
        int want = base + i;
        w.put_se(want - prev);
        prev = want;
    }
}

}  // namespace

// sizeId 0 (4x4) explicit list -> inverse diag_scan_4x4 raster placement.
TEST(ScalingListScan, Explicit4x4StoredRaster) {
    BitWriter w;

    // sizeId 0, matrixId 0: explicit ramp, value at scan pos i = 8 + i.
    w.put_flag(true);           // scaling_list_pred_mode_flag = 1
    put_ramp_list(w, 8, 16);

    // sizeId 0, matrixId 1..5: copy/default (pred_mode=0, delta=0).
    for (int m = 1; m < 6; m++) { w.put_flag(false); w.put_ue(0); }

    // sizeId 1..2, matrixId 0..5: default.
    for (int s = 1; s <= 2; s++)
        for (int m = 0; m < 6; m++) { w.put_flag(false); w.put_ue(0); }

    // sizeId 3, matrixId 0 and 3 (step 3): default.
    for (int m = 0; m < 6; m += 3) { w.put_flag(false); w.put_ue(0); }

    auto data = w.finish();
    BitstreamReader bs(data.data(), data.size());

    ScalingListData sld;
    ASSERT_TRUE(sld.parse(bs));

    // Each scan position i held value 8 + i and must land at raster cell
    // (xC,yC) = diag_scan_4x4[i], stride 4.
    for (int i = 0; i < 16; i++) {
        int xC = diag_scan_4x4[i][0];
        int yC = diag_scan_4x4[i][1];
        EXPECT_EQ(sld.scaling_list[0][0][yC * 4 + xC], 8 + i)
            << "scan pos " << i << " (xC=" << xC << ",yC=" << yC << ")";
    }

    // Untouched matrices keep flat-16 default for 4x4.
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(sld.scaling_list[0][1][i], 16);
    }
}

// sizeId 1 (8x8) explicit list -> inverse diag_scan_8x8 raster placement.
TEST(ScalingListScan, Explicit8x8StoredRaster) {
    BitWriter w;

    // sizeId 0, matrixId 0..5: default.
    for (int m = 0; m < 6; m++) { w.put_flag(false); w.put_ue(0); }

    // sizeId 1, matrixId 0: explicit ramp, value at scan pos i = 8 + i.
    w.put_flag(true);
    put_ramp_list(w, 8, 64);

    // sizeId 1, matrixId 1..5: default.
    for (int m = 1; m < 6; m++) { w.put_flag(false); w.put_ue(0); }

    // sizeId 2, matrixId 0..5: default.
    for (int m = 0; m < 6; m++) { w.put_flag(false); w.put_ue(0); }

    // sizeId 3, matrixId 0 and 3 (step 3): default.
    for (int m = 0; m < 6; m += 3) { w.put_flag(false); w.put_ue(0); }

    auto data = w.finish();
    BitstreamReader bs(data.data(), data.size());

    ScalingListData sld;
    ASSERT_TRUE(sld.parse(bs));

    // Each scan position i held value 8 + i and must land at raster cell
    // (xC,yC) = diag_scan_8x8[i], stride 8.
    for (int i = 0; i < 64; i++) {
        int xC = diag_scan_8x8[i][0];
        int yC = diag_scan_8x8[i][1];
        EXPECT_EQ(sld.scaling_list[1][0][yC * 8 + xC], 8 + i)
            << "scan pos " << i << " (xC=" << xC << ",yC=" << yC << ")";
    }

    // Spot-check raster cells the dequant read actually touches: the DC cell
    // (0,0) is scan pos 0 -> value 8; the bottom-right (7,7) is the last scan
    // pos 63 -> value 71.
    EXPECT_EQ(sld.scaling_list[1][0][0 * 8 + 0], 8);
    EXPECT_EQ(sld.scaling_list[1][0][7 * 8 + 7], 71);

    // Untouched 8x8 matrix keeps the (non-flat) default.
    EXPECT_EQ(sld.scaling_list[1][1][0], 16);
    EXPECT_EQ(sld.scaling_list[1][1][63], 115);
}
