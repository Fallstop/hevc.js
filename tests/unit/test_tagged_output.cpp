// P2 — tagged output and observability through the C API: every picture
// carries the tag of the feed call that delivered its first slice, output
// frames expose HEVC_FRAME_* flags, and hevc_decoder_get_stats() counts what
// was decoded, output, skipped and contained.

#include <gtest/gtest.h>

#include <cmath>
#include <map>

#include "test_util.h"
#include "wasm/hevc_api.h"

using namespace hevc;
using namespace hevc_test;

namespace {

struct Frame {
    int poc;
    double tag;
    int flags;
};

struct ApiDecoder {
    HEVCDecoder* dec = hevc_decoder_create();
    ~ApiDecoder() { hevc_decoder_destroy(dec); }

    std::vector<Frame> drained() {
        int count = 0;
        EXPECT_EQ(hevc_decoder_drain(dec, &count), HEVC_OK);
        std::vector<Frame> out;
        for (int i = 0; i < count; i++) {
            HEVCFrame f{};
            EXPECT_EQ(hevc_decoder_get_drained_frame(dec, i, &f), HEVC_OK);
            out.push_back({f.poc, f.tag, f.flags});
        }
        return out;
    }
    std::vector<Frame> flushed() {
        EXPECT_EQ(hevc_decoder_flush(dec), HEVC_OK);
        std::vector<Frame> out;
        HEVCFrame f{};
        for (int i = 0; hevc_decoder_get_drained_frame(dec, i, &f) == HEVC_OK; i++)
            out.push_back({f.poc, f.tag, f.flags});
        return out;
    }
    HEVCDecodeStats stats() {
        HEVCDecodeStats s{};
        EXPECT_EQ(hevc_decoder_get_stats(dec, &s), HEVC_OK);
        return s;
    }
};

// Feed one access unit per call, tagged with its index * 1000 + 0.5.
std::vector<Frame> decode_tagged(ApiDecoder& d, const std::vector<std::vector<uint8_t>>& aus) {
    std::vector<Frame> out;
    for (size_t i = 0; i < aus.size(); i++) {
        EXPECT_EQ(hevc_decoder_feed_tagged(d.dec, aus[i].data(), aus[i].size(),
                                           static_cast<double>(i) * 1000.0 + 0.5), HEVC_OK);
        for (const auto& f : d.drained()) out.push_back(f);
    }
    for (const auto& f : d.flushed()) out.push_back(f);
    return out;
}

// POC of the picture each access unit carries (decode order), -1 if none.
std::vector<int> au_pocs(const std::vector<std::vector<uint8_t>>& aus) {
    Decoder dec;
    std::vector<int> pocs;
    for (const auto& au : aus) {
        const Picture* before = dec.dpb().current_pic();
        dec.feed(au.data(), au.size());
        const Picture* now = dec.dpb().current_pic();
        pocs.push_back(now && now != before ? now->poc : -1);
        dec.drain();
    }
    return pocs;
}

}  // namespace

// On a B-frame stream output order differs from decode order; each output
// frame must still carry the tag of the access unit it was coded in.
TEST(TaggedOutput, TagsFollowPicturesThroughReordering) {
    for (const char* name : {"b_qcif_10f.265", "full_qcif_10f.265", "opengop_qcif_12f.265",
                             "multislice_wpp_qcif_8f.265"}) {
        auto data = read_file(fixture(name));
        ASSERT_FALSE(data.empty()) << name;
        auto aus = split_access_units(data);
        auto pocs = au_pocs(aus);

        ApiDecoder d;
        auto frames = decode_tagged(d, aus);
        ASSERT_FALSE(frames.empty()) << name;
        bool reordered = false;
        double prev_tag = -1;
        for (const auto& f : frames) {
            ASSERT_FALSE(std::isnan(f.tag)) << name;
            const size_t au = static_cast<size_t>(f.tag / 1000.0);
            ASSERT_LT(au, pocs.size()) << name;
            EXPECT_EQ(pocs[au], f.poc) << name << ": tag " << f.tag;
            EXPECT_EQ(f.tag, static_cast<double>(au) * 1000.0 + 0.5);
            EXPECT_EQ(f.flags, 0) << name;
            if (f.tag < prev_tag) reordered = true;
            prev_tag = f.tag;
        }
        if (std::string(name) == "b_qcif_10f.265") EXPECT_TRUE(reordered);
        auto st = d.stats();
        EXPECT_EQ(st.pictures_output, frames.size()) << name;
        EXPECT_EQ(st.pictures_decoded, frames.size()) << name;
        EXPECT_EQ(st.skipped_error + st.skipped_rasl + st.skipped_wait_irap + st.nal_errors, 0u) << name;
    }
}

TEST(TaggedOutput, UntaggedFeedsAndBatchDecodeYieldNaN) {
    auto data = read_file(fixture("p_qcif_10f.265"));
    ApiDecoder d;
    ASSERT_EQ(hevc_decoder_feed(d.dec, data.data(), data.size()), HEVC_OK);
    auto frames = d.drained();
    for (const auto& f : d.flushed()) frames.push_back(f);
    ASSERT_EQ(frames.size(), 10u);
    for (const auto& f : frames) EXPECT_TRUE(std::isnan(f.tag));

    ApiDecoder b;
    ASSERT_EQ(hevc_decoder_decode(b.dec, data.data(), data.size()), HEVC_OK);
    HEVCFrame f{};
    ASSERT_EQ(hevc_decoder_get_frame(b.dec, 0, &f), HEVC_OK);
    EXPECT_TRUE(std::isnan(f.tag));
}

// A stream joined mid-GOP skips pictures until the IRAP; RASL pictures of the
// opening CRA are skipped too. The frames that do come out keep their own
// tags, so a caller never mislabels a frame by counting.
TEST(TaggedOutput, SkippedPicturesDoNotShiftTags) {
    auto data = read_file(fixture("opengop_qcif_12f.265"));
    auto aus = split_access_units(data);
    auto pocs = au_pocs(aus);
    // Drop the access units up to (not including) the CRA, keeping the
    // parameter sets that open the first one, and start two pictures before it.
    size_t cra = 0;  // the last CRA
    {
        NalParser parser;
        for (size_t i = 0; i < aus.size(); i++)
            for (const auto& nal : parser.parse(aus[i].data(), aus[i].size()))
                if (nal.header.nal_unit_type == NalUnitType::CRA_NUT) cra = i;
    }
    ASSERT_GT(cra, 2u);
    auto ps = aus[0];
    NalParser parser;
    std::vector<uint8_t> params;
    for (const auto& nal : parser.parse(ps.data(), ps.size()))
        if (!is_vcl(nal.header.nal_unit_type)) { auto c = nal_chunk(ps, nal); params.insert(params.end(), c.begin(), c.end()); }
    std::vector<std::vector<uint8_t>> joined;
    std::vector<size_t> origin;
    for (size_t i = cra - 2; i < aus.size(); i++) {
        auto au = aus[i];
        if (joined.empty()) au.insert(au.begin(), params.begin(), params.end());
        joined.push_back(au);
        origin.push_back(i);
    }

    ApiDecoder d;
    auto frames = decode_tagged(d, joined);
    ASSERT_FALSE(frames.empty());
    for (const auto& f : frames) {
        const size_t j = static_cast<size_t>(f.tag / 1000.0);
        ASSERT_LT(j, origin.size());
        EXPECT_EQ(pocs[origin[j]], f.poc) << "tag " << f.tag;
    }
    auto st = d.stats();
    EXPECT_EQ(st.skipped_wait_irap, 2u);
    EXPECT_EQ(st.skipped_rasl, 3u);
    EXPECT_EQ(st.pictures_output, frames.size());
}

// An access unit lost entirely: the next picture's RPS names a picture the
// decoder never saw, and the frame says so.
TEST(TaggedOutput, MissingReferenceIsFlagged) {
    auto data = read_file(fixture("p_qcif_10f.265"));
    auto aus = split_access_units(data);
    ASSERT_GE(aus.size(), 6u);
    std::vector<std::vector<uint8_t>> lossy;
    for (size_t i = 0; i < aus.size(); i++)
        if (i != 4) lossy.push_back(aus[i]);

    ApiDecoder d;
    auto frames = decode_tagged(d, lossy);
    int flagged = 0;
    for (const auto& f : frames) {
        const size_t j = static_cast<size_t>(f.tag / 1000.0);
        if (j < 4) EXPECT_EQ(f.flags, 0);
        if (f.flags & HEVC_FRAME_MISSING_REF) flagged++;
    }
    EXPECT_GE(flagged, 1);
}
