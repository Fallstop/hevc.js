// P4 — Decoder::discontinuity(): the next CRA opens a new CVS like a BLA, no
// picture fed afterwards references one decoded before, and pictures awaiting
// output are released (not lost). Parameter sets survive, unlike reset().

#include <gtest/gtest.h>

#include "test_util.h"

using namespace hevc;
using namespace hevc_test;

namespace {

struct Out {
    std::vector<int32_t> pocs;
    std::vector<std::vector<uint8_t>> luma;
    std::vector<double> tags;
    void add(const std::vector<Picture*>& pics) {
        for (const Picture* p : pics) {
            pocs.push_back(p->poc);
            luma.push_back(p->plane_bytes[0]);
            tags.push_back(p->tag);
        }
    }
};

Out feed_aus(Decoder& dec, const std::vector<std::vector<uint8_t>>& aus, size_t from, size_t to,
             double tag_base = 0) {
    Out o;
    for (size_t i = from; i < to; i++) {
        dec.feed(aus[i].data(), aus[i].size(), tag_base + static_cast<double>(i));
        o.add(dec.drain());
    }
    return o;
}

Out standalone(const std::vector<uint8_t>& data) {
    Decoder dec;
    dec.decode(data.data(), data.size());
    Out o;
    o.add(dec.output_pictures());
    return o;
}

size_t last_cra(const std::vector<std::vector<uint8_t>>& aus) {
    NalParser parser;
    size_t idx = 0;
    for (size_t i = 0; i < aus.size(); i++)
        for (const auto& nal : parser.parse(aus[i].data(), aus[i].size()))
            if (nal.header.nal_unit_type == NalUnitType::CRA_NUT) idx = i;
    return idx;
}

}  // namespace

// Mid-stream the CRA would have NoRaslOutputFlag = 0 and output its RASL
// pictures. After discontinuity() it opens the sequence: the RASL pictures,
// whose references precede the cut, are skipped — exactly what decoding the
// stream cut at that CRA (the opengop_qcif_cra_first fixture) produces.
TEST(Discontinuity, NextCraIsHandledAsTheFirstPicture) {
    auto data = read_file(fixture("opengop_qcif_12f.265"));
    auto aus = split_access_units(data);
    const size_t cra = last_cra(aus);
    ASSERT_GT(cra, 0u);

    Decoder dec;
    Out before = feed_aus(dec, aus, 0, cra);
    dec.discontinuity();
    before.add(dec.drain());
    Out after = feed_aus(dec, aus, cra, aus.size());
    after.add(dec.flush());

    Out cut = standalone(read_file(fixture("opengop_qcif_cra_first.265")));
    ASSERT_EQ(cut.pocs.size(), 4u);
    EXPECT_EQ(after.pocs, cut.pocs);
    EXPECT_EQ(after.luma, cut.luma);
    EXPECT_EQ(dec.stats().skipped_rasl, 3u);
    // Every picture decoded before the call came out
    EXPECT_EQ(before.pocs.size(), dec.stats().pictures_decoded - after.pocs.size());
}

// Pictures still held for reordering when the discontinuity arrives are
// released to the next drain(), in output order.
TEST(Discontinuity, PicturesAwaitingOutputAreReleased) {
    auto data = read_file(fixture("b_qcif_10f.265"));
    auto aus = split_access_units(data);
    Decoder dec;
    Out got = feed_aus(dec, aus, 0, 6);
    const size_t drained_before = got.pocs.size();
    dec.discontinuity();
    got.add(dec.drain());
    EXPECT_GT(got.pocs.size(), drained_before);
    EXPECT_EQ(got.pocs.size(), dec.stats().pictures_decoded);
    for (size_t i = 1; i < got.pocs.size(); i++) EXPECT_LT(got.pocs[i - 1], got.pocs[i]);
    EXPECT_TRUE(dec.flush().empty());
}

// Non-IRAP pictures after the call would reference pictures before it: they
// are skipped until the next IRAP (default policy).
TEST(Discontinuity, NothingAfterwardsReferencesPicturesBefore) {
    auto data = read_file(fixture("p_qcif_10f.265"));
    auto aus = split_access_units(data);
    ASSERT_EQ(aus.size(), 10u);
    Decoder dec;
    feed_aus(dec, aus, 0, 5);
    dec.discontinuity();
    for (const auto& p : dec.dpb().pictures()) EXPECT_FALSE(p->is_reference());
    Out after = feed_aus(dec, aus, 5, 10);
    after.add(dec.flush());
    EXPECT_TRUE(after.pocs.empty());
    EXPECT_EQ(dec.stats().skipped_wait_irap, 5u);
}

// A splice: stream A, discontinuity, stream B from its IDR. Parameter sets of A
// stay known (B here is A again without its VPS/SPS/PPS), every frame comes out
// with its own tag, and B decodes exactly as on its own.
TEST(Discontinuity, SpliceKeepsParameterSetsAndTags) {
    auto data = read_file(fixture("p_qcif_10f.265"));
    auto aus = split_access_units(data);
    std::vector<std::vector<uint8_t>> b = aus;
    {
        NalParser parser;
        std::vector<uint8_t> first;
        for (const auto& nal : parser.parse(aus[0].data(), aus[0].size()))
            if (is_vcl(nal.header.nal_unit_type)) { auto c = nal_chunk(aus[0], nal); first.insert(first.end(), c.begin(), c.end()); }
        b[0] = first;
    }
    Decoder dec;
    Out a_out = feed_aus(dec, aus, 0, aus.size(), 0);
    dec.discontinuity();
    a_out.add(dec.drain());
    Out b_out = feed_aus(dec, b, 0, b.size(), 1000);
    b_out.add(dec.flush());

    Out ref = standalone(data);
    EXPECT_EQ(a_out.luma, ref.luma);
    EXPECT_EQ(b_out.luma, ref.luma);
    ASSERT_EQ(b_out.tags.size(), 10u);
    for (size_t i = 0; i < 10; i++) {
        EXPECT_EQ(a_out.tags[i], static_cast<double>(i));
        EXPECT_EQ(b_out.tags[i], 1000.0 + static_cast<double>(i));
    }
}

// With concealment, nothing decoded before the cut stands in for anything
// after it either — not as a substituted reference, not as the source of
// concealed CTUs. Whatever is fed after discontinuity() decodes exactly as it
// does on a fresh decoder that was given the parameter sets.
TEST(Discontinuity, ConcealmentNeverReachesAcrossTheCut) {
    const auto a = split_access_units(read_file(fixture("p_qcif_10f.265")));
    const auto ms = split_access_units(read_file(fixture("multislice_qcif_8f.265")));
    const auto cam = split_access_units(read_file(fixture("camera_qcif_12f.265")));

    auto non_vcl = [](const std::vector<uint8_t>& au) {
        NalParser parser;
        std::vector<uint8_t> out;
        for (const auto& nal : parser.parse(au.data(), au.size()))
            if (!is_vcl(nal.header.nal_unit_type)) { auto c = nal_chunk(au, nal); out.insert(out.end(), c.begin(), c.end()); }
        return out;
    };
    // The multi-slice IDR without its last slice
    std::vector<uint8_t> idr_cut;
    {
        NalParser parser;
        const auto nals = parser.parse(ms[0].data(), ms[0].size());
        size_t nvcl = 0, k = 0;
        for (const auto& nal : nals) nvcl += is_vcl(nal.header.nal_unit_type);
        for (const auto& nal : nals) {
            if (is_vcl(nal.header.nal_unit_type) && ++k == nvcl) break;
            auto c = nal_chunk(ms[0], nal);
            idr_cut.insert(idr_cut.end(), c.begin(), c.end());
        }
    }
    // Camera stream joined at its first P picture
    std::vector<std::vector<uint8_t>> cam_join = {non_vcl(cam[0])};
    cam_join.insert(cam_join.end(), cam.begin() + 1, cam.end());

    struct Case {
        const char* what;
        const std::vector<std::vector<uint8_t>>* before;
        std::vector<std::vector<uint8_t>> after;
        ErrorPolicy policy;
    };
    ErrorPolicy conceal;
    conceal.conceal_slice_errors = true;
    ErrorPolicy both = conceal;
    both.decode_before_irap = true;
    const Case cases[] = {
        {"IDR missing its last slice", &ms, {idr_cut}, conceal},
        {"stream joined at a P picture", &a, cam_join, both},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        Decoder fresh;
        fresh.set_error_policy(c.policy);
        Out want = feed_aus(fresh, c.after, 0, c.after.size());
        want.add(fresh.flush());
        ASSERT_FALSE(want.luma.empty());

        Decoder dec;
        dec.set_error_policy(c.policy);
        feed_aus(dec, *c.before, 0, c.before->size());
        dec.discontinuity();
        dec.drain();
        Out got = feed_aus(dec, c.after, 0, c.after.size());
        got.add(dec.flush());
        EXPECT_EQ(got.pocs, want.pocs);
        EXPECT_EQ(got.luma, want.luma);
    }
}
