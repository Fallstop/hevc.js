#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <vector>

#include "bitstream/nal_unit.h"
#include "common/types.h"
#include "decoding/decoder.h"

using namespace hevc;

// Helper: read a fixture file into a byte vector
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

// The opengop fixture is an IDR-GOP followed by an open-GOP CRA whose leading
// pictures are RASL. From the START the CRA is mid-stream (NoRaslOutputFlag == 0)
// so the RASL pictures are decoded and output — that is covered by the
// byte-exact oracle test. Here we exercise the RANDOM-ACCESS decision path
// (feature F1): tuning in AT the CRA must treat it as the first picture of a CVS
// (NoRaslOutputFlag == 1), SKIP its RASL leading pictures, and decode the
// trailing pictures, producing output bit-identical to the corresponding tail
// of a full from-start decode.

// Find the byte offset at which to "tune in": the first NAL of the access unit
// that contains the mid-stream CRA (i.e. the parameter-set group immediately
// preceding the CRA, or the CRA itself if it has no preceding parameter sets).
static size_t find_cra_tunein_offset(const std::vector<uint8_t>& stream) {
    NalParser parser;
    auto nals = parser.parse(stream.data(), stream.size());
    size_t cra_idx = nals.size();
    for (size_t i = 0; i < nals.size(); i++) {
        if (is_cra(nals[i].header.nal_unit_type)) { cra_idx = i; break; }
    }
    EXPECT_LT(cra_idx, nals.size()) << "fixture has no CRA";
    if (cra_idx >= nals.size()) return 0;
    // Walk back over the non-VCL parameter sets / SEI that belong to the CRA's AU.
    size_t start = cra_idx;
    while (start > 0) {
        auto t = nals[start - 1].header.nal_unit_type;
        if (t == NalUnitType::VPS_NUT || t == NalUnitType::SPS_NUT ||
            t == NalUnitType::PPS_NUT || t == NalUnitType::PREFIX_SEI ||
            t == NalUnitType::AUD_NUT) {
            start--;
        } else {
            break;
        }
    }
    return nals[start].offset;
}

TEST(RandomAccess, MidStreamCraTuneInSkipsRaslAndMatchesFullDecode) {
    std::string path = std::string(FIXTURES_DIR) + "/opengop_cra_rasl.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    // Full from-start decode (reference).
    Decoder full;
    ASSERT_EQ(full.decode(stream.data(), stream.size()), DecodeStatus::OK);
    auto full_pics = full.output_pictures();
    ASSERT_GT(full_pics.size(), 0u);

    // Tune in at the CRA: feed only the bytes from the CRA's AU onward.
    size_t off = find_cra_tunein_offset(stream);
    ASSERT_GT(off, 0u) << "expected a mid-stream CRA (not the first AU)";
    ASSERT_LT(off, stream.size());

    Decoder tune;
    ASSERT_EQ(tune.decode(stream.data() + off, stream.size() - off),
              DecodeStatus::OK);
    auto tune_pics = tune.output_pictures();
    ASSERT_GT(tune_pics.size(), 0u);

    // RASL pictures must have been skipped: the tune-in output is strictly
    // shorter than the count of VCL pictures from the CRA onward in the full
    // stream (which would include the RASL leading pictures).
    EXPECT_LT(tune_pics.size(), full_pics.size())
        << "tune-in did not drop any leading pictures";

    // Every tune-in frame must be byte-identical to a frame in the full decode.
    // (The CRA tune-in anchor maps to the CRA picture of the full decode; the
    //  trailing pictures follow in display order.) Match the tune-in sequence
    //  against the tail of the full decode of equal length.
    ASSERT_LE(tune_pics.size(), full_pics.size());
    size_t base = full_pics.size() - tune_pics.size();
    for (size_t i = 0; i < tune_pics.size(); i++) {
        const Picture* a = tune_pics[i];
        const Picture* b = full_pics[base + i];
        EXPECT_EQ(a->plane_bytes[0], b->plane_bytes[0])
            << "Y plane mismatch at tune-in frame " << i;
        EXPECT_EQ(a->plane_bytes[1], b->plane_bytes[1])
            << "Cb plane mismatch at tune-in frame " << i;
        EXPECT_EQ(a->plane_bytes[2], b->plane_bytes[2])
            << "Cr plane mismatch at tune-in frame " << i;
    }
}

// Wait-for-random-access-point: after a fresh start/reset, non-IRAP VCL pictures
// reference frames the decoder never decoded and must be skipped until the first
// IRAP. We exercise the reset()-driven path (a realistic camera tune-in where
// parameter sets were delivered once and are retained): decode the IDR GOP +
// CRA, reset() keeping parameter sets, then feed only the post-CRA region (which
// starts at a RASL and contains no further IRAP). Everything must be skipped, so
// no pictures are output.
TEST(RandomAccess, WaitsForIrapAfterResetWhenStartingOnNonIrap) {
    std::string path = std::string(FIXTURES_DIR) + "/opengop_cra_rasl.265";
    auto stream = read_file(path);
    ASSERT_FALSE(stream.empty()) << "Cannot read " << path;

    NalParser parser;
    auto nals = parser.parse(stream.data(), stream.size());
    // Offset of the first VCL picture after the CRA (its first RASL).
    size_t post_cra_off = 0;
    bool seen_cra = false;
    for (const auto& nal : nals) {
        if (is_cra(nal.header.nal_unit_type)) { seen_cra = true; continue; }
        if (seen_cra && is_vcl(nal.header.nal_unit_type)) {
            post_cra_off = nal.offset;
            break;
        }
    }
    ASSERT_GT(post_cra_off, 0u);

    Decoder dec;
    // Decode up to (and including) the CRA so the parameter sets are active.
    ASSERT_EQ(dec.decode(stream.data(), post_cra_off), DecodeStatus::OK);
    ASSERT_GT(dec.output_pictures().size(), 0u);

    // Reset, keeping parameter sets — re-arms wait-for-IRAP.
    dec.reset(/*clear_parameter_sets=*/false);
    ASSERT_EQ(dec.decode(stream.data() + post_cra_off, stream.size() - post_cra_off),
              DecodeStatus::OK);
    // No IRAP appears after the CRA, so every VCL picture is skipped while
    // waiting for a random access point: nothing is decoded/output.
    EXPECT_EQ(dec.output_pictures().size(), 0u)
        << "decoder did not wait for an IRAP before outputting";
}
