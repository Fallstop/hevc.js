// §C.5.2 output process — pictures are bumped at the points the spec defines
// (before the current picture is stored, §C.5.2.2, and after it is decoded,
// §C.5.2.3) into an output queue that drain()/flush() hand over.
//
// Ported from upstream's drain-order, pic_output_flag and RASL-output fixes,
// re-implemented against this fork's decoder-level NoRaslOutputFlag.

#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <vector>

#include "syntax/parameter_sets.h"
#include "test_util.h"

using namespace hevc;
using namespace hevc_test;

namespace {

std::vector<std::pair<int32_t, int32_t>> batch_order(const std::vector<uint8_t>& data) {
    Decoder dec;
    EXPECT_EQ(dec.decode(data.data(), data.size()), DecodeStatus::OK);
    std::vector<std::pair<int32_t, int32_t>> out;
    for (const Picture* p : dec.output_pictures()) out.push_back({p->cvs_id, p->poc});
    return out;
}

std::vector<std::pair<int32_t, int32_t>> per_nal_order(const std::vector<uint8_t>& data) {
    NalParser parser;
    auto nals = parser.parse(data.data(), data.size());
    Decoder dec;
    std::vector<std::pair<int32_t, int32_t>> out;
    for (const auto& nal : nals) {
        auto chunk = nal_chunk(data, nal);
        EXPECT_EQ(dec.feed(chunk.data(), chunk.size()), DecodeStatus::OK);
        for (const Picture* p : dec.drain()) out.push_back({p->cvs_id, p->poc});
    }
    for (const Picture* p : dec.flush()) out.push_back({p->cvs_id, p->poc});
    return out;
}

// Rewrites `data` so the PPS sets output_flag_present_flag and every slice
// carries pic_output_flag = 1, except the pictures whose decode-order index is
// in `suppress`, which get 0. Single-slice pictures only. The slice header
// grows by the one bit it gates; the byte-aligned slice data is copied through.
std::vector<uint8_t> patch_pic_output_flag(const std::vector<uint8_t>& data,
                                           const std::set<int>& suppress) {
    NalParser parser;
    auto nals = parser.parse(data.data(), data.size());
    ParameterSetManager psm;
    for (const auto& nal : nals) {
        auto t = nal.header.nal_unit_type;
        if (t == NalUnitType::VPS_NUT || t == NalUnitType::SPS_NUT || t == NalUnitType::PPS_NUT)
            psm.process_nal(nal);
    }

    std::vector<uint8_t> out;
    int pic_index = 0;
    for (const auto& nal : nals) {
        std::vector<uint8_t> rbsp = nal.rbsp;
        const auto type = nal.header.nal_unit_type;
        if (type == NalUnitType::PPS_NUT) {
            BitstreamReader bs(rbsp.data(), rbsp.size());
            bs.read_ue();
            bs.read_ue();
            bs.read_flag();
            set_bit(rbsp, bs.bits_read(), 1);  // output_flag_present_flag
        } else if (is_vcl(type)) {
            BitstreamReader probe(rbsp.data(), rbsp.size());
            EXPECT_TRUE(probe.read_flag()) << "single-slice pictures only";
            if (is_irap(type)) probe.read_flag();
            const uint32_t pps_id = probe.read_ue();
            const PPS* pps = psm.get_pps(static_cast<int>(pps_id));
            const SPS* sps = pps ? psm.get_sps(static_cast<int>(pps->pps_seq_parameter_set_id)) : nullptr;
            if (!pps || !sps) { ADD_FAILURE() << "missing parameter sets"; return {}; }
            for (int i = 0; i < pps->num_extra_slice_header_bits; i++) probe.read_flag();
            probe.read_ue();  // slice_type
            const size_t insert_at = probe.bits_read();

            SliceHeader sh;
            BitstreamReader full(rbsp.data(), rbsp.size());
            EXPECT_TRUE(sh.parse(full, *sps, *pps, type, nal.header.TemporalId()));
            const size_t header_bits = full.bits_read();

            const bool output = suppress.count(pic_index) == 0;
            pic_index++;

            // byte_alignment() ends the header: one '1' then zeros. The
            // inserted bit shifts it by one, so drop one trailing zero — or,
            // when there was none, pad a whole byte of zeros after the '1'.
            BitWriter w;
            w.copy_bits(rbsp, 0, insert_at);
            w.put_bit(output ? 1 : 0);
            w.copy_bits(rbsp, insert_at, header_bits - 1);
            if (get_bit(rbsp, header_bits - 1)) w.put_bit(1);
            while (w.bits() % 8) w.put_bit(0);
            std::vector<uint8_t> patched = w.take();
            patched.insert(patched.end(), rbsp.begin() + static_cast<long>(header_bits / 8), rbsp.end());
            rbsp = std::move(patched);
        }
        auto nal_bytes = make_nal(data[nal.offset], data[nal.offset + 1], rbsp);
        out.insert(out.end(), nal_bytes.begin(), nal_bytes.end());
    }
    return out;
}

// POC of each picture in decode order.
std::vector<int32_t> decode_order_pocs(const std::vector<uint8_t>& data) {
    Decoder dec;
    std::vector<int32_t> pocs;
    for (const auto& au : split_access_units(data)) {
        const Picture* before = dec.dpb().current_pic();
        dec.feed(au.data(), au.size());
        const Picture* now = dec.dpb().current_pic();
        if (now && now != before) pocs.push_back(now->poc);
        dec.drain();
    }
    return pocs;
}

const char* kReorderFixtures[] = {
    "b_qcif_10f.265", "full_qcif_10f.265", "full_qcif_10f_10bit.265",
    "p_qcif_10f.265", "opengop_qcif_12f.265", "opengop_cra_rasl.265",
    "sei_recovery_point.265",
};

}  // namespace

// Draining after every NAL / every access unit must output exactly the batch
// order. Before the §C.5.2 rework, "DPB full" counted storage that had already
// been released and drained B-frame streams in decode order.
TEST(OutputProcess, IncrementalDrainMatchesBatchOrder) {
    for (const char* name : kReorderFixtures) {
        auto data = read_file(fixture(name));
        ASSERT_FALSE(data.empty()) << name;
        auto batch = batch_order(data);
        ASSERT_FALSE(batch.empty()) << name;

        EXPECT_EQ(per_nal_order(data), batch) << name << " (per NAL)";

        Decoder dec;
        auto inc = decode_per_au(dec, data);
        std::vector<std::pair<int32_t, int32_t>> au_order;
        for (size_t i = 0; i < inc.pocs.size(); i++) au_order.push_back({inc.cvs[i], inc.pocs[i]});
        EXPECT_EQ(au_order, batch) << name << " (per AU)";
    }
}

// §C.5.2.3: with sps_max_num_reorder_pics == 0 a decoded picture is output by
// the drain() that follows its own feed() — no extra frame of latency.
TEST(OutputProcess, NoReorderMeansNoOutputLatency) {
    auto data = read_file(fixture("p_qcif_10f.265"));
    ASSERT_FALSE(data.empty());

    NalParser parser;
    ParameterSetManager psm;
    for (const auto& nal : parser.parse(data.data(), data.size()))
        if (nal.header.nal_unit_type == NalUnitType::SPS_NUT) psm.process_nal(nal);
    const SPS* sps = psm.get_sps(0);
    ASSERT_NE(sps, nullptr);
    ASSERT_EQ(sps->sub_layer_ordering[sps->sps_max_sub_layers_minus1].max_num_reorder_pics, 0u)
        << "fixture no longer low-delay; pick another";

    Decoder dec;
    size_t decoded = 0, drained = 0;
    for (const auto& au : split_access_units(data)) {
        const Picture* before = dec.dpb().current_pic();
        dec.feed(au.data(), au.size());
        if (dec.dpb().current_pic() != before) decoded++;
        drained += dec.drain().size();
        EXPECT_EQ(drained, decoded);
    }
    EXPECT_EQ(decoded, 10u);
}

// The DPB holds at most MaxDpbSize pictures plus the one being decoded.
TEST(OutputProcess, DpbStaysBounded) {
    for (const char* name : kReorderFixtures) {
        auto data = read_file(fixture(name));
        Decoder dec;
        auto out = decode_per_au(dec, data);
        EXPECT_LE(out.max_dpb, static_cast<size_t>(DPB::kMaxDpbPictures + 1)) << name;
    }
}

// §C.3.1: a picture with PicOutputFlag = 0 is decoded (and may be referenced)
// but never output; every other picture is unaffected.
TEST(OutputProcess, PicOutputFlagIsHonoured) {
    auto data = read_file(fixture("full_qcif_10f.265"));
    ASSERT_FALSE(data.empty());
    auto order = decode_order_pocs(data);
    ASSERT_EQ(order.size(), 10u);

    // Neutral patch: every picture still output, pixel-identical.
    auto neutral = patch_pic_output_flag(data, {});
    Decoder ref, neu;
    ASSERT_EQ(ref.decode(data.data(), data.size()), DecodeStatus::OK);
    ASSERT_EQ(neu.decode(neutral.data(), neutral.size()), DecodeStatus::OK);
    auto ref_pics = ref.output_pictures();
    auto neu_pics = neu.output_pictures();
    ASSERT_EQ(ref_pics.size(), neu_pics.size());
    for (size_t i = 0; i < ref_pics.size(); i++)
        EXPECT_EQ(ref_pics[i]->plane_bytes[0], neu_pics[i]->plane_bytes[0]) << "frame " << i;

    const std::set<int> suppress = {2, 5};
    auto patched = patch_pic_output_flag(data, suppress);
    std::set<int32_t> hidden = {order[2], order[5]};

    Decoder batch;
    ASSERT_EQ(batch.decode(patched.data(), patched.size()), DecodeStatus::OK);
    auto pics = batch.output_pictures();
    EXPECT_EQ(pics.size(), 8u);
    for (const Picture* p : pics) {
        EXPECT_EQ(hidden.count(p->poc), 0u) << "suppressed POC " << p->poc << " was output";
        for (const Picture* r : ref_pics)
            if (r->poc == p->poc) EXPECT_EQ(r->plane_bytes[0], p->plane_bytes[0]) << "POC " << p->poc;
    }

    Decoder inc;
    auto out = decode_per_au(inc, patched);
    EXPECT_EQ(out.pocs.size(), 8u);
    for (int32_t poc : out.pocs) EXPECT_EQ(hidden.count(poc), 0u);
}

// §8.1: RASL pictures of an IRAP with NoRaslOutputFlag = 1 are never output.
// opengop_qcif_12f has its CRA mid-stream (RASL decodable, 12 out of 12);
// opengop_qcif_cra_first is cut at that CRA (7 coded, 4 out — as ffmpeg).
TEST(RaslOutput, OpeningCraDropsItsRaslPictures) {
    auto full = read_file(fixture("opengop_qcif_12f.265"));
    auto cut = read_file(fixture("opengop_qcif_cra_first.265"));
    ASSERT_FALSE(full.empty());
    ASSERT_FALSE(cut.empty());
    EXPECT_EQ(batch_order(full).size(), 12u);
    EXPECT_EQ(batch_order(cut).size(), 4u);
    EXPECT_EQ(per_nal_order(cut).size(), 4u);
}

// A stream cut at its first RASL opens before any IRAP: those RASL pictures
// are skipped while waiting for the IRAP, and the CRA that follows opens the
// CVS — so its own RASL pictures are dropped too.
TEST(RaslOutput, StreamOpeningOnRaslOutputsOnlyDecodablePictures) {
    auto full = read_file(fixture("opengop_qcif_12f.265"));
    NalParser parser;
    auto nals = parser.parse(full.data(), full.size());
    std::vector<uint8_t> cut;
    bool in_rasl_tail = false;
    for (const auto& nal : nals) {
        auto t = nal.header.nal_unit_type;
        if (!is_vcl(t)) {
            if (!in_rasl_tail) { auto c = nal_chunk(full, nal); cut.insert(cut.end(), c.begin(), c.end()); }
            continue;
        }
        if (is_rasl(t)) in_rasl_tail = true;
        if (in_rasl_tail) { auto c = nal_chunk(full, nal); cut.insert(cut.end(), c.begin(), c.end()); }
    }
    // Parameter sets are at the front of the stream and were kept above.
    auto order = batch_order(cut);
    EXPECT_EQ(order.size(), 4u);
}
