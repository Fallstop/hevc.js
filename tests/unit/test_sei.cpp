#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <vector>

#include "bitstream/nal_unit.h"
#include "bitstream/bitstream_reader.h"
#include "common/types.h"
#include "syntax/sei.h"
#include "decoding/decoder.h"

using namespace hevc;

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

// --- Synthetic payload tests (exercise the se(v)/u(1) decode directly) ---

// Build a one-message sei_rbsp(): payloadType=6 (recovery_point), then a payload
// of `payload` bytes, then the rbsp trailing bits (0x80). payloadSize uses the
// single-byte form (< 255).
static std::vector<uint8_t> make_recovery_point_rbsp(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> rbsp;
    rbsp.push_back(6);                                   // payloadType
    rbsp.push_back(static_cast<uint8_t>(payload.size()));// payloadSize
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
    rbsp.push_back(0x80);                                // rbsp_trailing_bits
    return rbsp;
}

TEST(Sei, RecoveryPointZeroPocCnt) {
    // payload byte 0xd0 = 1101 0000:
    //   recovery_poc_cnt se(v): leading '1' -> ue 0 -> se 0
    //   exact_match_flag u(1): '1' -> true
    //   broken_link_flag u(1): '0' -> false
    auto rbsp = make_recovery_point_rbsp({0xd0});
    BitstreamReader bs(rbsp.data(), rbsp.size());
    SeiMessages sei = parse_sei(bs);

    ASSERT_TRUE(sei.has_recovery_point);
    EXPECT_EQ(sei.recovery_point.recovery_poc_cnt, 0);
    EXPECT_TRUE(sei.recovery_point.exact_match_flag);
    EXPECT_FALSE(sei.recovery_point.broken_link_flag);
}

TEST(Sei, RecoveryPointPositivePocCnt) {
    // recovery_poc_cnt = 3 -> se(v) code maps 3 -> ue 5 -> Exp-Golomb '00110'.
    // Then exact_match_flag=1, broken_link_flag=0.
    // Bits: 0 0 1 1 0 | 1 | 0 -> 00110 10  -> 0011 0100 = 0x34.
    auto rbsp = make_recovery_point_rbsp({0x34});
    BitstreamReader bs(rbsp.data(), rbsp.size());
    SeiMessages sei = parse_sei(bs);

    ASSERT_TRUE(sei.has_recovery_point);
    EXPECT_EQ(sei.recovery_point.recovery_poc_cnt, 3);
    EXPECT_TRUE(sei.recovery_point.exact_match_flag);
    EXPECT_FALSE(sei.recovery_point.broken_link_flag);
}

TEST(Sei, RecoveryPointNegativePocCnt) {
    // recovery_poc_cnt = -2 -> se(v) code maps -2 -> ue 4 -> '00101'.
    // Then exact_match_flag=0, broken_link_flag=1.
    // Bits: 0 0 1 0 1 | 0 | 1 -> 00101 01 -> 0010 1011... pad: 0x2A | trailing
    // 00101 0 1 = 0010 1010 1 (9 bits). First byte 0x2A, then high bit set.
    auto rbsp = make_recovery_point_rbsp({0x2A, 0x80});
    BitstreamReader bs(rbsp.data(), rbsp.size());
    SeiMessages sei = parse_sei(bs);

    ASSERT_TRUE(sei.has_recovery_point);
    EXPECT_EQ(sei.recovery_point.recovery_poc_cnt, -2);
    EXPECT_FALSE(sei.recovery_point.exact_match_flag);
    EXPECT_TRUE(sei.recovery_point.broken_link_flag);
}

TEST(Sei, SkipsUnknownPayloadTypeThenParsesRecoveryPoint) {
    // Two messages: an unknown type (payloadType=99) with 3 junk bytes, then a
    // recovery_point. The unknown one must be skipped by payloadSize so the
    // recovery_point is still found.
    std::vector<uint8_t> rbsp;
    rbsp.push_back(99);                 // unknown payloadType
    rbsp.push_back(3);                  // payloadSize
    rbsp.insert(rbsp.end(), {0xAB, 0xCD, 0xEF});
    rbsp.push_back(6);                  // recovery_point
    rbsp.push_back(1);                  // payloadSize
    rbsp.push_back(0xd0);              // recovery_poc_cnt=0, exact=1, broken=0
    rbsp.push_back(0x80);              // rbsp_trailing_bits

    BitstreamReader bs(rbsp.data(), rbsp.size());
    SeiMessages sei = parse_sei(bs);
    ASSERT_TRUE(sei.has_recovery_point);
    EXPECT_EQ(sei.recovery_point.recovery_poc_cnt, 0);
}

TEST(Sei, ParsesUserDataUnregisteredUuidAndData) {
    std::vector<uint8_t> payload;
    for (int i = 0; i < 16; i++) payload.push_back(static_cast<uint8_t>(i)); // uuid
    payload.insert(payload.end(), {0xDE, 0xAD, 0xBE, 0xEF});                 // data

    std::vector<uint8_t> rbsp;
    rbsp.push_back(5);                                    // user_data_unregistered
    rbsp.push_back(static_cast<uint8_t>(payload.size())); // payloadSize
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
    rbsp.push_back(0x80);

    BitstreamReader bs(rbsp.data(), rbsp.size());
    SeiMessages sei = parse_sei(bs);
    ASSERT_TRUE(sei.has_user_data_unregistered);
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(sei.user_data_unregistered.uuid_iso_iec_11578[i], i);
    ASSERT_EQ(sei.user_data_unregistered.data.size(), 4u);
    EXPECT_EQ(sei.user_data_unregistered.data[0], 0xDE);
    EXPECT_EQ(sei.user_data_unregistered.data[3], 0xEF);
}

// --- Real-bitstream test: confirm PREFIX_SEI recovery_point present and feed
//     the SEI payload to our parser. ---

TEST(Sei, RealFixtureHasRecoveryPointPrefixSei) {
    std::string path = std::string(FIXTURES_DIR) + "/sei_recovery_point.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    NalParser parser;
    auto nals = parser.parse(stream.data(), stream.size());

    bool found = false;
    for (const auto& nal : nals) {
        if (nal.header.nal_unit_type != NalUnitType::PREFIX_SEI) continue;
        BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());
        SeiMessages sei = parse_sei(bs);
        if (sei.has_recovery_point) {
            found = true;
            // x265 idr-recovery-sei emits recovery_poc_cnt=0 for the IDR.
            EXPECT_EQ(sei.recovery_point.recovery_poc_cnt, 0);
        }
    }
    EXPECT_TRUE(found) << "fixture has no recovery_point PREFIX_SEI";
}

// --- Full decoder path: the recovery point is surfaced via the accessor. ---

TEST(Sei, DecoderSurfacesRecoveryPoint) {
    std::string path = std::string(FIXTURES_DIR) + "/sei_recovery_point.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    Decoder dec;
    ASSERT_EQ(dec.decode(stream.data(), stream.size()), DecodeStatus::OK);

    EXPECT_TRUE(dec.has_recovery_point());
    EXPECT_EQ(dec.recovery_poc_cnt(), 0);
    // Associated picture is the IDR (POC 0), recovery_poc_cnt 0 -> recovery POC 0.
    EXPECT_EQ(dec.recovery_point_poc(), 0);

    // reset() is a tune-in and must clear the recovery-point state.
    dec.reset();
    EXPECT_FALSE(dec.has_recovery_point());
}
