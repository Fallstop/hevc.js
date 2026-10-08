// P5 — picture assembly. Slice segments are grouped into pictures by
// first_slice_segment_in_pic_flag, address order, PPS and POC — not by NAL
// adjacency — so suffix SEI / filler data between slices no longer split a
// picture, a picture may span feed() calls, a slice of the next picture (whose
// first slice was lost) is not merged into the current one, and NAL units of
// other layers, reserved VCL types and NALs with the forbidden bit are ignored.

#include <gtest/gtest.h>

#include "ps_builder.h"

using namespace hevc;
using namespace hevc_test;

namespace {

const char* kMultiSlice[] = {"multislice_qcif_8f.265", "multislice_wpp_qcif_8f.265"};

struct Output {
    std::vector<int32_t> pocs;
    std::vector<std::vector<uint8_t>> luma;
};

Output batch(const std::vector<uint8_t>& data, DecodeStats* stats = nullptr) {
    Decoder dec;
    EXPECT_EQ(dec.decode(data.data(), data.size()), DecodeStatus::OK);
    Output o;
    for (const Picture* p : dec.output_pictures()) {
        o.pocs.push_back(p->poc);
        o.luma.push_back(p->plane_bytes[0]);
    }
    if (stats) *stats = dec.stats();
    return o;
}

// Rebuild a stream NAL by NAL, letting `edit` insert, drop or replace units.
// edit(index, nal, out) appends whatever should take the NAL's place.
template <typename Edit>
std::vector<uint8_t> rewrite(const std::vector<uint8_t>& data, Edit edit) {
    NalParser parser;
    auto nals = parser.parse(data.data(), data.size());
    std::vector<uint8_t> out;
    for (size_t i = 0; i < nals.size(); i++) edit(i, nals[i], nal_chunk(data, nals[i]), out);
    return out;
}

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) {
    out.insert(out.end(), v.begin(), v.end());
}

bool first_slice(const NalUnit& nal) {
    return is_vcl(nal.header.nal_unit_type) && !nal.rbsp.empty() && (nal.rbsp[0] & 0x80);
}

}  // namespace

TEST(PictureAssembly, InterleavedNonVclDoesNotSplitPictures) {
    for (const char* name : kMultiSlice) {
        auto data = read_file(fixture(name));
        ASSERT_FALSE(data.empty()) << name;
        BitWriter sei;
        sei.put_bits(5, 8);   // user_data_unregistered
        sei.put_bits(17, 8);
        for (int i = 0; i < 17; i++) sei.put_bits(0x42, 8);
        sei.trailing();
        const auto suffix_sei = make_nal(nal_h0(NalUnitType::SUFFIX_SEI), kNalH1, sei.take());
        const auto filler = make_nal(nal_h0(NalUnitType::FD_NUT), kNalH1, {0xFF, 0xFF, 0x80});
        const auto unspecified = make_nal(static_cast<uint8_t>(50 << 1), kNalH1, {0x12, 0x80});

        auto mixed = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                       std::vector<uint8_t>& out) {
            append(out, chunk);
            if (is_vcl(nal.header.nal_unit_type)) {
                append(out, suffix_sei);
                append(out, filler);
                append(out, unspecified);
            }
        });
        Output clean = batch(data);
        ASSERT_EQ(clean.pocs.size(), 8u) << name;
        Output got = batch(mixed);
        EXPECT_EQ(got.pocs, clean.pocs) << name;
        EXPECT_EQ(got.luma, clean.luma) << name;
    }
}

// The slices of one picture may arrive in separate feed() calls (an RTP
// depacketizer hands over NAL units one at a time).
TEST(PictureAssembly, PicturesMaySpanFeedCalls) {
    for (const char* name : kMultiSlice) {
        auto data = read_file(fixture(name));
        Output clean = batch(data);

        NalParser parser;
        Decoder dec;
        Output got;
        for (const auto& nal : parser.parse(data.data(), data.size())) {
            auto chunk = nal_chunk(data, nal);
            ASSERT_EQ(dec.feed(chunk.data(), chunk.size()), DecodeStatus::OK);
            for (const Picture* p : dec.drain()) {
                got.pocs.push_back(p->poc);
                got.luma.push_back(p->plane_bytes[0]);
            }
        }
        for (const Picture* p : dec.flush()) {
            got.pocs.push_back(p->poc);
            got.luma.push_back(p->plane_bytes[0]);
        }
        EXPECT_EQ(got.pocs, clean.pocs) << name;
        EXPECT_EQ(got.luma, clean.luma) << name;
        EXPECT_EQ(dec.stats().skipped_error, 0u) << name;
    }
}

// Lose the middle slice of the 4th picture: under the default policy that
// picture is dropped and decoding resumes at the next IRAP — and the pictures
// before it are untouched.
TEST(PictureAssembly, LostMiddleSliceDropsThePictureByDefault) {
    auto data = read_file(fixture("multislice_qcif_8f.265"));
    int pic = -1, slice = 0;
    auto lossy = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                   std::vector<uint8_t>& out) {
        if (is_vcl(nal.header.nal_unit_type)) {
            if (first_slice(nal)) { pic++; slice = 0; } else { slice++; }
            if (pic == 3 && slice == 1) return;
        }
        append(out, chunk);
    });
    DecodeStats stats;
    Output clean = batch(data);
    Output got = batch(lossy, &stats);
    EXPECT_LT(got.pocs.size(), clean.pocs.size());
    EXPECT_GE(stats.skipped_error, 1u);
    for (size_t i = 0; i < got.pocs.size(); i++) {
        for (size_t j = 0; j < clean.pocs.size(); j++)
            if (clean.pocs[j] == got.pocs[i]) EXPECT_EQ(got.luma[i], clean.luma[j]) << "POC " << got.pocs[i];
    }
}

// Lose the FIRST slice of a picture: its remaining slices start a new
// picture (POC / address order tell them apart) instead of being decoded into
// the previous picture, which must come out unchanged.
TEST(PictureAssembly, SlicesOfAPictureMissingItsFirstSliceAreNotMergedBackwards) {
    auto data = read_file(fixture("multislice_qcif_8f.265"));
    int pic = -1;
    auto lossy = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                   std::vector<uint8_t>& out) {
        if (is_vcl(nal.header.nal_unit_type) && first_slice(nal)) {
            pic++;
            if (pic == 3) return;
        }
        append(out, chunk);
    });
    Output clean = batch(data);
    Output got = batch(lossy);
    // Every picture that comes out matches the clean decode.
    for (size_t i = 0; i < got.pocs.size(); i++)
        for (size_t j = 0; j < clean.pocs.size(); j++)
            if (clean.pocs[j] == got.pocs[i]) EXPECT_EQ(got.luma[i], clean.luma[j]) << "POC " << got.pocs[i];
}

// NAL units of another layer, reserved VCL types and NAL units with the
// forbidden bit set never reach the decoding process.
TEST(PictureAssembly, ForeignLayerReservedAndForbiddenNalsAreIgnored) {
    auto data = read_file(fixture("multislice_qcif_8f.265"));
    auto noisy = rewrite(data, [&](size_t, const NalUnit& nal, const std::vector<uint8_t>& chunk,
                                   std::vector<uint8_t>& out) {
        append(out, chunk);
        if (is_vcl(nal.header.nal_unit_type) || nal.header.nal_unit_type == NalUnitType::SPS_NUT) {
            // Same NAL in layer 1 (nuh_layer_id in the low bit of byte 0 and
            // the top 5 bits of byte 1)
            auto layer1 = chunk;
            layer1[5] = static_cast<uint8_t>(layer1[5] | 0x08);
            append(out, layer1);
            // Same NAL with the forbidden_zero_bit set
            auto forbidden = chunk;
            forbidden[4] = static_cast<uint8_t>(forbidden[4] | 0x80);
            append(out, forbidden);
            // A reserved VCL type carrying the slice payload
            auto reserved = chunk;
            reserved[4] = static_cast<uint8_t>((reserved[4] & 0x81) | (12 << 1));
            append(out, reserved);
        }
    });
    DecodeStats stats;
    Output clean = batch(data);
    Output got = batch(noisy, &stats);
    EXPECT_EQ(got.pocs, clean.pocs);
    EXPECT_EQ(got.luma, clean.luma);
    EXPECT_GT(stats.nal_errors, 0u);  // the forbidden-bit copies
}
