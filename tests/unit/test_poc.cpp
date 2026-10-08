// §8.3.1 picture order count — the prevTid0Pic that PicOrderCntMsb is
// predicted from.

#include <gtest/gtest.h>

#include "decoding/dpb.h"
#include "syntax/slice_header.h"
#include "syntax/sps.h"

using namespace hevc;

namespace {

struct PocFixture {
    DPB dpb;
    SPS sps;
    PocFixture() { sps.log2_max_pic_order_cnt_lsb_minus4 = 0; }  // MaxPicOrderCntLsb 16

    int32_t poc(NalUnitType type, uint32_t lsb, uint8_t tid = 0, bool no_rasl = true) {
        SliceHeader sh;
        sh.slice_pic_order_cnt_lsb = lsb;
        return dpb.derive_poc(sh, sps, type, tid, no_rasl);
    }
};

}  // namespace

// POC_A_Bossen: a TRAIL_N at TemporalId 0 is a sub-layer non-reference picture
// and must not become prevTid0Pic. IDR 0, TRAIL_R 8, TRAIL_N lsb 0 (POC 16),
// TRAIL_N lsb 1: predicted from POC 8, so POC 1 — not 17.
TEST(Poc, SubLayerNonReferenceAtTid0IsNotPrevTid0Pic) {
    PocFixture f;
    EXPECT_EQ(f.poc(NalUnitType::IDR_W_RADL, 0), 0);
    EXPECT_EQ(f.poc(NalUnitType::RADL_R, 9), -7);
    EXPECT_EQ(f.poc(NalUnitType::TRAIL_R, 8), 8);
    EXPECT_EQ(f.poc(NalUnitType::TRAIL_N, 0), 16);
    EXPECT_EQ(f.poc(NalUnitType::TRAIL_N, 1), 1);
}

TEST(Poc, HigherSubLayersAndLeadingPicturesAreNotPrevTid0Pic) {
    PocFixture f;
    EXPECT_EQ(f.poc(NalUnitType::IDR_W_RADL, 0), 0);
    EXPECT_EQ(f.poc(NalUnitType::TRAIL_R, 6), 6);
    EXPECT_EQ(f.poc(NalUnitType::TSA_R, 12, /*tid=*/1), 12);
    EXPECT_EQ(f.poc(NalUnitType::RASL_R, 13, 0, false), 13);
    // Still predicted from POC 6: lsb 1 is within half a cycle of 6.
    EXPECT_EQ(f.poc(NalUnitType::TRAIL_R, 1), 1);
}

// A CRA that opens the bitstream with a non-zero LSB is prevTid0Pic with that
// LSB, so trailing pictures that wrapped the LSB still follow it.
TEST(Poc, OpeningCraKeepsItsLsbForTheNextPicture) {
    PocFixture f;
    EXPECT_EQ(f.poc(NalUnitType::CRA_NUT, 14), 14);
    EXPECT_EQ(f.poc(NalUnitType::TRAIL_R, 2), 18);
}
