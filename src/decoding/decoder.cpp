#include "decoding/decoder.h"
#include "bitstream/nal_unit.h"
#include "bitstream/bitstream_reader.h"
#include "syntax/sei.h"
#include "filters/deblocking.h"
#include "filters/sao.h"
#include "common/debug.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace hevc {

namespace {

// Bits needed for values 0..x-1 (u(v) width of slice_segment_address)
int ceil_log2(int x) {
    int bits = 0;
    while ((1 << bits) < x) bits++;
    return bits;
}

}  // namespace

// ============================================================
// NAL dispatch
// ============================================================

DecodeStatus Decoder::decode(const uint8_t* data, size_t size) {
    NalParser parser;
    auto nals = parser.parse(data, size);

    HEVC_LOG(PARSE, "Decoded %zu NAL units", nals.size());

    for (const auto& nal : nals) process_nal(nal);
    return DecodeStatus::OK;
}

void Decoder::process_nal(const NalUnit& nal) {
    const auto& h = nal.header;

    // §7.4.2.2: forbidden_zero_bit is 0 and nuh_temporal_id_plus1 is not; a NAL
    // breaking either is corrupt, and its payload is not worth parsing.
    if (h.forbidden_zero_bit || h.nuh_temporal_id_plus1 == 0) {
        stats_.nal_errors++;
        return;
    }
    // Single-layer decoder (§F.7): NAL units of other layers are not ours. An
    // enhancement layer's parameter sets would otherwise replace the base
    // layer's (they share ids), and its slices are not decodable here.
    if (h.nuh_layer_id > 0) return;

    const NalUnitType type = h.nal_unit_type;
    const uint8_t t = static_cast<uint8_t>(type);

    if (t <= 31) {
        // §7.4.2.2: reserved VCL types (RSV_VCL_N10..R15, RSV_IRAP_22/23,
        // RSV_VCL24..31) are ignored by decoders of this version.
        if ((t >= 10 && t <= 15) || t >= 22) return;
        process_vcl(nal);
        return;
    }

    switch (type) {
    case NalUnitType::VPS_NUT:
    case NalUnitType::SPS_NUT:
    case NalUnitType::PPS_NUT: {
        // A parameter set starts a new access unit (§7.4.2.4.4); the picture
        // being assembled can receive no further slices, and must not see its
        // parameter sets change under it.
        finish_picture();
        // A corrupt or truncated parameter set must not throw out of feed()
        // and lose the rest of the buffer. The manager only stores a set that
        // parsed completely, so the previous valid one stays in force.
        bool ok = false;
        try {
            ok = ps_mgr_.process_nal(nal);
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            stats_.nal_errors++;
            HEVC_LOG(PARSE, "Parameter set NAL type %d rejected; keeping the previous one",
                     static_cast<int>(type));
        }
        break;
    }
    case NalUnitType::AUD_NUT:
        finish_picture();
        break;
    case NalUnitType::EOS_NUT:
    case NalUnitType::EOB_NUT:
        // §8.1: after an end of sequence the next CRA is the first picture of a
        // CVS (NoRaslOutputFlag == 1, RASL discarded). EOB behaves the same.
        finish_picture();
        handle_cra_as_first_ = true;
        break;
    case NalUnitType::PREFIX_SEI:
    case NalUnitType::SUFFIX_SEI: {
        // Prefix SEI can sit between the slices of a picture (decoding-unit
        // info), so it does not end the picture being assembled.
        // A malformed SEI is contained like a malformed parameter set.
        try {
            BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());
            SeiMessages sei = parse_sei(bs);
            // recovery_point is a prefix-only message (§D.1): bind it to the
            // next picture in decoding order. A PREFIX_SEI precedes its
            // associated picture, so defer until that picture's POC is known.
            if (sei.has_recovery_point && type == NalUnitType::PREFIX_SEI) {
                has_pending_recovery_point_ = true;
                pending_recovery_poc_cnt_ = sei.recovery_point.recovery_poc_cnt;
            }
        } catch (...) {
            stats_.nal_errors++;
        }
        break;
    }
    default:
        // Filler data, reserved and unspecified non-VCL types: no effect on
        // decoding, and they do not split a picture (§7.4.2.4.4).
        break;
    }
}

// ============================================================
// Picture assembly
// ============================================================

bool Decoder::peek_slice(const NalUnit& nal, SlicePeek& out) const {
    if (nal.rbsp.empty()) return false;
    BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());
    const NalUnitType nut = nal.header.nal_unit_type;

    out = SlicePeek{};
    out.first_slice = bs.read_flag();
    if (is_irap(nut)) bs.read_flag();  // no_output_of_prior_pics_flag
    out.pps_id = bs.read_ue();
    if (out.pps_id > 63) return false;
    const PPS* pps = ps_mgr_.get_pps(static_cast<int>(out.pps_id));
    if (!pps) return false;
    const SPS* sps = ps_mgr_.get_sps(static_cast<int>(pps->pps_seq_parameter_set_id));
    if (!sps) return false;

    if (!out.first_slice) {
        if (pps->dependent_slice_segments_enabled_flag) out.dependent = bs.read_flag();
        out.address = bs.read_bits(ceil_log2(sps->PicSizeInCtbsY));
        if (out.address >= static_cast<uint32_t>(sps->PicSizeInCtbsY)) return false;
    }
    out.address_ts = static_cast<int>(out.address);
    if (pps->derived_valid &&
        pps->CtbAddrRsToTs.size() == static_cast<size_t>(sps->PicSizeInCtbsY))
        out.address_ts = pps->CtbAddrRsToTs[out.address];

    if (!out.dependent) {
        for (int i = 0; i < pps->num_extra_slice_header_bits; i++) bs.read_flag();
        bs.read_ue();  // slice_type
        if (pps->output_flag_present_flag) bs.read_flag();
        if (sps->separate_colour_plane_flag) bs.read_bits(2);
        if (!is_idr(nut)) {
            out.poc_lsb = bs.read_bits(static_cast<int>(sps->log2_max_pic_order_cnt_lsb_minus4) + 4);
            out.has_poc = true;
        }
    }
    return true;
}

void Decoder::process_vcl(const NalUnit& nal) {
    SlicePeek pk;
    bool peeked = false;
    try {
        peeked = peek_slice(nal, pk);
    } catch (...) {
        peeked = false;
    }

    if (!peeked) {
        // The slice header does not even parse far enough to tell which
        // picture it belongs to. Inside a picture, count it as a lost slice
        // of that picture; otherwise the picture it started is lost.
        if (pend_.mode == PendingMode::DECODING) {
            pend_.error = true;
            pend_.prev_segment_ok = false;
        } else if (pend_.mode == PendingMode::NONE) {
            stats_.skipped_error++;
            skip_to_next_irap_ = true;
        }
        return;
    }

    if (pend_.mode != PendingMode::NONE) {
        // §7.4.2.4.5: the segments of a picture follow its first segment in
        // increasing address order and share its PPS and POC. Anything else
        // starts a new picture — typically one whose first slice was lost.
        const bool same_picture =
            !pk.first_slice && pk.pps_id == pend_.pps_id &&
            pk.address_ts >= pend_.next_ctb_ts &&
            (!pk.has_poc || !pend_.has_poc || pk.poc_lsb == pend_.poc_lsb);
        if (same_picture) {
            if (pend_.mode == PendingMode::SKIPPING) {
                pend_.next_ctb_ts = pk.address_ts + 1;
            } else {
                decode_slice(nal, pk, nullptr);
                if (pend_.mode == PendingMode::DECODING && pend_.sps &&
                    pend_.next_ctb_ts >= pend_.sps->PicSizeInCtbsY)
                    finish_picture();
            }
            return;
        }
        finish_picture();
    }

    if (pk.dependent) {
        // A dependent segment with no picture to continue: its independent
        // segment (and the picture's POC/RPS) were lost.
        HEVC_LOG(PARSE, "Orphan dependent slice segment dropped (addr=%u)", pk.address);
        stats_.skipped_error++;
        skip_to_next_irap_ = true;
        begin_skip(pk, nal.header.nal_unit_type);
        return;
    }
    start_picture(nal, pk);
}

void Decoder::begin_skip(const SlicePeek& peek, NalUnitType nut) {
    pend_.mode = PendingMode::SKIPPING;
    pend_.nut = nut;
    pend_.pps_id = peek.pps_id;
    pend_.has_poc = peek.has_poc;
    pend_.poc_lsb = peek.poc_lsb;
    pend_.next_ctb_ts = peek.address_ts + 1;
    pend_.pic = nullptr;
}

void Decoder::start_picture(const NalUnit& nal, const SlicePeek& peek) {
    const NalUnitType nut = nal.header.nal_unit_type;

    SliceHeader first_sh;
    bool parsed = false;
    try {
        parsed = ps_mgr_.parse_slice_header(first_sh, nal);
    } catch (...) {
        parsed = false;
    }
    const SPS* sps = ps_mgr_.active_sps();
    const PPS* pps = ps_mgr_.active_pps();
    if (!parsed || !sps || !pps) {
        // The picture's POC / RPS are unknown: nothing that follows can be
        // predicted reliably. Resynchronize at the next IRAP.
        HEVC_LOG(PARSE, "First slice header undecodable — resyncing at next IRAP%s", "");
        stats_.skipped_error++;
        skip_to_next_irap_ = true;
        begin_skip(peek, nut);
        return;
    }

    // §8.1 — random access decisions, evaluated before touching the DPB.
    const bool isIRAP = is_irap(nut);
    if (isIRAP) {
        // NoRaslOutputFlag: 1 for IDR/BLA, and for a CRA that is the first
        // picture of the bitstream / first after EOS_NUT / first after reset().
        // A mid-stream CRA gets 0 (its RASL pictures are decodable & output).
        bool no_rasl_output = is_idr(nut) || is_bla(nut) ||
                              (is_cra(nut) && (handle_cra_as_first_ || no_irap_yet_));
        prev_irap_no_rasl_output_ = no_rasl_output;
        no_irap_yet_ = false;
        handle_cra_as_first_ = false;
        // F4 error resync: an IRAP starts a clean CVS — references are self
        // contained, so we can resume decoding here. Clear the error-skip arm.
        skip_to_next_irap_ = false;
    } else {
        // F4 error resync: after a picture failed, drop every non-IRAP picture
        // until the next IRAP. Their references may be the corrupted picture
        // (or pictures that depended on it); recover at the next keyframe.
        if (skip_to_next_irap_) {
            HEVC_LOG(PARSE, "Skipping non-IRAP VCL (resyncing after decode error)%s", "");
            stats_.skipped_error++;
            begin_skip(peek, nut);
            return;
        }
        // §8.1 wait-for-random-access-point: until the first IRAP after a fresh
        // start/reset, non-IRAP VCL pictures reference frames we never decoded.
        if (no_irap_yet_) {
            HEVC_LOG(PARSE, "Skipping non-IRAP VCL (waiting for random access point)%s", "");
            stats_.skipped_wait_irap++;
            begin_skip(peek, nut);
            return;
        }
        // §8.1: RASL pictures associated with an IRAP whose NoRaslOutputFlag == 1
        // are undecodable — their references were discarded. Skip them.
        if (is_rasl(nut) && prev_irap_no_rasl_output_) {
            HEVC_LOG(PARSE, "Skipping RASL (associated IRAP has NoRaslOutputFlag=1)%s", "");
            stats_.skipped_rasl++;
            begin_skip(peek, nut);
            return;
        }
    }
    const bool no_rasl_output_flag = isIRAP && prev_irap_no_rasl_output_;

    HEVC_LOG(PARSE, "Decoding picture: %dx%d type=%d QP=%d",
             sps->pic_width_in_luma_samples, sps->pic_height_in_luma_samples,
             static_cast<int>(first_sh.slice_type), first_sh.SliceQpY);

    // §8.3.1 — POC derivation (from the first slice of the picture)
    int32_t poc = dpb_.derive_poc(first_sh, *sps, nut,
                                   nal.header.TemporalId(), no_rasl_output_flag);

    // §D.3.8 — bind a pending recovery_point SEI to this (its associated)
    // picture: output is reliable from POC + recovery_poc_cnt onward.
    if (has_pending_recovery_point_) {
        has_recovery_point_ = true;
        recovery_poc_cnt_ = pending_recovery_poc_cnt_;
        recovery_point_poc_ = poc + pending_recovery_poc_cnt_;
        has_pending_recovery_point_ = false;
    }

    // §8.3.2 — RPS derivation and picture marking. Runs before the current
    // picture is stored: the previous picture is an ordinary DPB entry now.
    dpb_.clear_current();
    dpb_.derive_rps(first_sh, *sps, nut, poc, no_rasl_output_flag);

    // §C.5.2.2 — output/removal of pictures before the current one is stored.
    output_limits_ = DPB::output_limits(*sps);
    dpb_.bump_before_decode(output_limits_, isIRAP && no_rasl_output_flag);

    // Allocate picture in DPB
    ChromaFormat fmt = static_cast<ChromaFormat>(sps->chroma_format_idc);
    Picture* pic = dpb_.alloc_picture(
        static_cast<int>(sps->pic_width_in_luma_samples),
        static_cast<int>(sps->pic_height_in_luma_samples),
        fmt, sps->BitDepthY, sps->BitDepthC);
    pic->poc = poc;
    pic->tag = feed_tag_;
    pic->flags = 0;
    // Marked "needed for output" only once the picture is decoded (§C.5.2.3)
    pic->needed_for_output = false;

    // §8.1: IRAP with NoRaslOutputFlag == 1 starts a new CVS. This now covers a
    // CRA at the start of the bitstream / after EOS / after reset() too, not
    // just IDR/BLA — so random-access tune-in produces a fresh CVS boundary.
    if (isIRAP && no_rasl_output_flag)
        cvs_id_++;
    pic->cvs_id = cvs_id_;

    // Set conformance window
    if (sps->conformance_window_flag) {
        pic->conf_win_left = sps->conf_win_left_offset * sps->SubWidthC;
        pic->conf_win_right = sps->conf_win_right_offset * sps->SubWidthC;
        pic->conf_win_top = sps->conf_win_top_offset * sps->SubHeightC;
        pic->conf_win_bottom = sps->conf_win_bottom_offset * sps->SubHeightC;
    }

    // ---- Per-picture scratch: resize + reset every grid ----
    // Allocate CU info grid (min-CB granularity)
    int cuGridW = sps->PicWidthInMinCbsY;
    int cuGridH = sps->PicHeightInMinCbsY;
    cu_info_buf_.resize(cuGridW * cuGridH);
    std::fill(cu_info_buf_.begin(), cu_info_buf_.end(), CUInfo{});

    // Intra mode grid uses min-TB granularity (4x4) to support NxN sub-PU
    int modeGridW = sps->pic_width_in_luma_samples / sps->MinTbSizeY;
    int modeGridH = sps->pic_height_in_luma_samples / sps->MinTbSizeY;
    intra_mode_buf_.resize(modeGridW * modeGridH);
    std::fill(intra_mode_buf_.begin(), intra_mode_buf_.end(), 1); // DC default
    chroma_mode_buf_.resize(modeGridW * modeGridH);
    std::fill(chroma_mode_buf_.begin(), chroma_mode_buf_.end(), 0); // Planar default

    // Motion info grid at min-PU (4x4) granularity for inter MV storage
    motion_info_buf_.resize(modeGridW * modeGridH);
    std::fill(motion_info_buf_.begin(), motion_info_buf_.end(), PUMotionInfo{});
    // Allocate compact motion info in the Picture itself (for TMVP by future frames)
    pic->motion_info_buf.resize(modeGridW * modeGridH);
    std::fill(pic->motion_info_buf.begin(), pic->motion_info_buf.end(), Picture::PUMotionInfoCompact{});
    pic->motion_info_stride = modeGridW;

    // Phase 6: filter grids at min-TB (4x4) granularity
    cbf_luma_buf_.resize(modeGridW * modeGridH, 0);
    std::fill(cbf_luma_buf_.begin(), cbf_luma_buf_.end(), 0);
    log2_tu_size_buf_.resize(modeGridW * modeGridH, sps->CtbLog2SizeY);
    std::fill(log2_tu_size_buf_.begin(), log2_tu_size_buf_.end(),
              static_cast<uint8_t>(sps->CtbLog2SizeY));
    edge_flags_v_buf_.resize(modeGridW * modeGridH, 0);
    std::fill(edge_flags_v_buf_.begin(), edge_flags_v_buf_.end(), 0);
    edge_flags_h_buf_.resize(modeGridW * modeGridH, 0);
    std::fill(edge_flags_h_buf_.begin(), edge_flags_h_buf_.end(), 0);

    // Phase 6: SAO params per CTU
    int ctbGridSize = sps->PicWidthInCtbsY * sps->PicHeightInCtbsY;
    sao_params_buf_.resize(ctbGridSize);
    std::fill(sao_params_buf_.begin(), sao_params_buf_.end(), DecodingContext::SaoParams{});

    // Phase 10: slice number per CTU (none yet), and the decoded-CTU map
    slice_idx_buf_.resize(ctbGridSize);
    std::fill(slice_idx_buf_.begin(), slice_idx_buf_.end(), DecodingContext::kNoSlice);
    ctb_decoded_buf_.resize(ctbGridSize);
    std::fill(ctb_decoded_buf_.begin(), ctb_decoded_buf_.end(), 0);

    // ---- Pending-picture state ----
    pend_.mode = PendingMode::DECODING;
    pend_.nut = nut;
    pend_.pps_id = peek.pps_id;
    pend_.has_poc = peek.has_poc;
    pend_.poc_lsb = peek.poc_lsb;
    pend_.next_ctb_ts = 0;
    pend_.pic = pic;
    pend_.sps = sps;
    pend_.pps = pps;
    pend_.headers.clear();
    pend_.slices.clear();
    pend_.last_independent = nullptr;
    pend_.prev_segment_ok = false;
    pend_.error = false;
    pend_.pic_output_flag = first_sh.pic_output_flag;

    // Setup decoding context (shared across all slices of the picture).
    // The CABAC engine persists across segments: a dependent segment continues
    // the contexts its predecessor ended with (§9.3.2.4).
    DecodingContext& ctx = pend_.ctx;
    ctx = DecodingContext{};
    ctx.wpp_contexts_available = false;
    ctx.thread_pool = &thread_pool_;
    ctx.sps = sps;
    ctx.pps = pps;
    ctx.cabac = &pend_.cabac;
    ctx.pic = pic;
    ctx.dpb = &dpb_;
    ctx.cu_info = cu_info_buf_.data();
    ctx.cu_info_stride = cuGridW;
    ctx.intra_pred_mode_y = intra_mode_buf_.data();
    ctx.intra_pred_mode_c = chroma_mode_buf_.data();
    ctx.intra_pred_mode_stride = modeGridW;
    ctx.motion_info = motion_info_buf_.data();
    ctx.motion_info_stride = modeGridW;
    ctx.cbf_luma_grid = cbf_luma_buf_.data();
    ctx.log2_tu_size_grid = log2_tu_size_buf_.data();
    ctx.edge_flags_v = edge_flags_v_buf_.data();
    ctx.edge_flags_h = edge_flags_h_buf_.data();
    ctx.filter_grid_stride = modeGridW;
    ctx.sao_params = sao_params_buf_.data();
    ctx.sao_params_stride = sps->PicWidthInCtbsY;
    ctx.sao_backup = sao_backup_;
    ctx.slice_idx = slice_idx_buf_.data();
    ctx.ctb_decoded = ctb_decoded_buf_.data();

    // A picture whose first slice was lost cannot be completed: under the
    // default policy it is dropped like any other picture with an error.
    if (!first_sh.first_slice_segment_in_pic_flag) pend_.error = true;

    decode_slice(nal, peek, &first_sh);
    if (pend_.mode == PendingMode::DECODING &&
        pend_.next_ctb_ts >= sps->PicSizeInCtbsY)
        finish_picture();
}

void Decoder::record_slice_refs(int slice_number) {
    auto& tables = pend_.pic->ref_tables;
    if (tables.size() <= static_cast<size_t>(slice_number))
        tables.resize(static_cast<size_t>(slice_number) + 1);
    Picture::RefPocTable& t = tables[static_cast<size_t>(slice_number)];
    t = Picture::RefPocTable{};
    t.count[0] = static_cast<uint8_t>(std::min(dpb_.num_ref_list0(), 16));
    t.count[1] = static_cast<uint8_t>(std::min(dpb_.num_ref_list1(), 16));
    for (int i = 0; i < t.count[0]; i++) {
        const Picture* p = dpb_.ref_pic_list0(i);
        t.poc[0][i] = p ? p->poc : -999999;
        t.is_long_term[0][i] = dpb_.ref_is_long_term0(i);
    }
    for (int i = 0; i < t.count[1]; i++) {
        const Picture* p = dpb_.ref_pic_list1(i);
        t.poc[1][i] = p ? p->poc : -999999;
        t.is_long_term[1][i] = dpb_.ref_is_long_term1(i);
    }
}

void Decoder::decode_slice(const NalUnit& nal, const SlicePeek& peek, SliceHeader* parsed) {
    const SPS* sps = pend_.sps;
    const PPS* pps = pend_.pps;

    pend_.headers.emplace_back();
    SliceHeader& sh = pend_.headers.back();
    if (parsed) {
        sh = *parsed;
        if (parsed->active_rps == &parsed->st_rps) sh.active_rps = &sh.st_rps;
    } else {
        bool ok = false;
        try {
            ok = ps_mgr_.parse_slice_header(sh, nal);
        } catch (...) {
            ok = false;
        }
        // The grouping keys matched, so the PPS id is the pending picture's;
        // a header that still fails to parse loses this segment (and, for an
        // independent one, the dependent segments that would continue it).
        if (!ok || ps_mgr_.active_pps() != pps) {
            pend_.headers.pop_back();
            pend_.error = true;
            pend_.prev_segment_ok = false;
            if (!peek.dependent) pend_.last_independent = nullptr;
            pend_.next_ctb_ts = std::max(pend_.next_ctb_ts, peek.address_ts + 1);
            return;
        }
    }

    int slice_number;
    if (sh.dependent_slice_segment_flag) {
        // §7.4.7.1: inherit from the independent segment of the same slice.
        // Its CABAC state continues from the previous segment, so that one
        // must have decoded too.
        if (!pend_.last_independent || !pend_.prev_segment_ok) {
            HEVC_LOG(PARSE, "Dependent slice segment without its predecessor dropped (addr=%u)",
                     sh.slice_segment_address);
            pend_.headers.pop_back();
            pend_.error = true;
            pend_.prev_segment_ok = false;
            pend_.next_ctb_ts = std::max(pend_.next_ctb_ts, peek.address_ts + 1);
            return;
        }
        sh.inherit_from(*pend_.last_independent);
        slice_number = static_cast<int>(pend_.slices.size()) - 1;
    } else {
        if (pend_.slices.size() >= DecodingContext::kNoSlice) {
            pend_.headers.pop_back();
            pend_.error = true;
            pend_.prev_segment_ok = false;
            pend_.last_independent = nullptr;
            return;
        }
        slice_number = static_cast<int>(pend_.slices.size());
        pend_.slices.push_back(&sh);
        pend_.last_independent = &sh;

        // §8.3.4 / §8.3.5 — reference lists and ColPic, per independent slice
        // (slices of a picture share the RPS but may size or reorder their
        // lists differently).
        dpb_.construct_ref_pic_lists(sh, *sps, *pps);
        dpb_.derive_colpic(sh);
        record_slice_refs(slice_number);
        if (sh.slice_type != SliceType::I && dpb_.has_missing_curr_refs())
            pend_.pic->flags |= PICTURE_MISSING_REF;
    }

    // Under the default policy a picture with an error is dropped anyway; do
    // not spend time decoding the rest of it.
    if (pend_.error) {
        pend_.prev_segment_ok = false;
        pend_.next_ctb_ts = std::max(pend_.next_ctb_ts, peek.address_ts + 1);
        return;
    }

    decode_slice_data(nal, sh, slice_number);
}

void Decoder::decode_slice_data(const NalUnit& nal, SliceHeader& sh, int slice_number) {
    const SPS* sps = pend_.sps;
    const PPS* pps = pend_.pps;
    DecodingContext& ctx = pend_.ctx;

    ctx.sh = &sh;
    ctx.current_slice_idx = slice_number;
    ctx.slice_headers = pend_.slices.data();
    ctx.num_slices = static_cast<int>(pend_.slices.size());

    HEVC_LOG(PARSE, "Slice %d: addr=%d type=%d QP=%d dependent=%d",
             slice_number, sh.slice_segment_address, static_cast<int>(sh.slice_type),
             sh.SliceQpY, sh.dependent_slice_segment_flag);

    // Slice data starts right after the header (parse() recorded where).
    BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());
    bs.seek_to_byte(sh.slice_data_byte_offset);

    // Coded size of the slice header, for converting entry-point offsets
    // (counted in coded bytes, EP bytes included) to RBSP positions. The
    // RBSP starts after the 2-byte NAL header; EP positions count from there.
    size_t slice_header_coded_size = sh.slice_data_byte_offset;
    for (size_t ep : nal.epb_positions) {
        if (ep < slice_header_coded_size + 2)
            slice_header_coded_size++;
    }

    const int start_ts = pps->CtbAddrRsToTs[sh.slice_segment_address];
    bool ok = false;
    try {
        ok = decode_slice_segment_data(ctx, bs, nal.epb_positions, slice_header_coded_size);
    } catch (...) {
        ok = false;
    }

    // Extent actually decoded: consecutive CTBs (tile-scan order) from the
    // segment start that this slice decoded.
    int end_ts = start_ts;
    while (end_ts < sps->PicSizeInCtbsY) {
        const int rs = pps->CtbAddrTsToRs[end_ts];
        if (!ctb_decoded_buf_[rs] || slice_idx_buf_[rs] != slice_number) break;
        end_ts++;
    }
    pend_.next_ctb_ts = std::max(pend_.next_ctb_ts, std::max(end_ts, start_ts + 1));
    pend_.prev_segment_ok = ok;
    if (!ok) {
        pend_.error = true;
        HEVC_LOG(PARSE, "Slice %d data failed to decode", slice_number);
    }
}

void Decoder::fail_picture() {
    dpb_.drop_current();
    skip_to_next_irap_ = true;
    stats_.skipped_error++;
    pend_.mode = PendingMode::NONE;
    pend_.pic = nullptr;
    HEVC_LOG(PARSE, "Picture decode failed — resyncing at next IRAP%s", "");
}

void Decoder::finish_picture() {
    if (pend_.mode == PendingMode::SKIPPING) {
        pend_.mode = PendingMode::NONE;
        return;
    }
    if (pend_.mode != PendingMode::DECODING) return;

    const SPS* sps = pend_.sps;
    Picture* pic = pend_.pic;
    DecodingContext& ctx = pend_.ctx;

    int decoded = 0;
    for (uint8_t d : ctb_decoded_buf_) decoded += d;
    if (pend_.error || decoded != sps->PicSizeInCtbsY || pend_.slices.empty()) {
        fail_picture();
        return;
    }

    // §8.7: In-loop filters — deblocking then SAO (once, after all slices).
    // Filter parameters are looked up per CTB through ctx.slice_headers.
    ctx.sh = pend_.slices.back();
    ctx.slice_headers = pend_.slices.data();
    ctx.num_slices = static_cast<int>(pend_.slices.size());
    try {
        apply_deblocking(ctx);
        apply_sao(ctx);
    } catch (...) {
        fail_picture();
        return;
    }

    // Store motion info in the Picture for TMVP access by future frames
    const size_t n = motion_info_buf_.size();
    for (size_t i = 0; i < n; i++) {
        auto& src = motion_info_buf_[i];
        auto& dst = pic->motion_info_buf[i];
        dst.mv_x[0] = src.mv[0].x; dst.mv_y[0] = src.mv[0].y;
        dst.mv_x[1] = src.mv[1].x; dst.mv_y[1] = src.mv[1].y;
        dst.ref_idx[0] = src.ref_idx[0]; dst.ref_idx[1] = src.ref_idx[1];
        dst.pred_flag[0] = src.pred_flag[0]; dst.pred_flag[1] = src.pred_flag[1];
    }
    // Which slice (and so which reference table) each CTB belongs to
    pic->ctb_log2_size = sps->CtbLog2SizeY;
    pic->width_in_ctbs = sps->PicWidthInCtbsY;
    if (pic->ref_tables.size() > 1)
        pic->ctb_slice.assign(slice_idx_buf_.begin(), slice_idx_buf_.end());
    else
        pic->ctb_slice.clear();

    // §8.1 step 4: mark current picture as short-term reference
    dpb_.mark_current_as_short_term_ref();

    // §C.5.2.3: the decoded picture is marked "needed for output" iff
    // PicOutputFlag is 1, then the "additional bumping" runs.
    pic->needed_for_output = pend_.pic_output_flag;
    dpb_.bump_after_decode(output_limits_);

    stats_.pictures_decoded++;
    pend_.mode = PendingMode::NONE;
    pend_.pic = nullptr;
}

// ============================================================
// Output / control
// ============================================================

DecodeStats Decoder::stats() const {
    DecodeStats s = stats_;
    s.pictures_output = static_cast<uint32_t>(dpb_.pictures_bumped());
    return s;
}

DecodeStatus Decoder::feed(const uint8_t* data, size_t size) {
    return feed(data, size, std::numeric_limits<double>::quiet_NaN());
}

DecodeStatus Decoder::feed(const uint8_t* data, size_t size, double tag) {
    feed_tag_ = tag;
    DecodeStatus status = decode(data, size);
    feed_tag_ = std::numeric_limits<double>::quiet_NaN();
    return status;
}

std::vector<Picture*> Decoder::drain() {
    // Pictures are bumped into the output queue during decoding (§C.5.2.2 /
    // §C.5.2.3); drain hands over whatever is ready. A picture still being
    // assembled (its slices may arrive in the next feed) is not finished here.
    return dpb_.take_output();
}

std::vector<Picture*> Decoder::flush() {
    finish_picture();
    dpb_.bump_all();
    return dpb_.take_output();
}

std::vector<Picture*> Decoder::output_pictures() {
    // Batch mode is end-of-stream: finish the last picture, release every
    // remaining picture, then return all the DPB holds for output, sorted by
    // CVS then POC. Pictures with PicOutputFlag = 0 are skipped, as the
    // incremental path skips them.
    finish_picture();
    dpb_.bump_all();
    std::vector<Picture*> out;
    for (auto& pic : dpb_.pictures()) {
        if (!pic->needed_for_output && !pic->output_queued) continue;
        out.push_back(pic.get());
    }
    std::sort(out.begin(), out.end(),
              [](const Picture* a, const Picture* b) {
                  if (a->cvs_id != b->cvs_id) return a->cvs_id < b->cvs_id;
                  return a->poc < b->poc;
              });
    return out;
}

void Decoder::discontinuity() {
    finish_picture();
    dpb_.discontinuity();
    // §8.1: the next IRAP opens a new CVS — a CRA takes NoRaslOutputFlag = 1
    no_irap_yet_ = true;
    handle_cra_as_first_ = true;
    skip_to_next_irap_ = false;
    // A recovery point announced before the discontinuity belongs to a
    // picture that will not come.
    has_pending_recovery_point_ = false;
}

void Decoder::reset(bool clear_parameter_sets) {
    pend_.mode = PendingMode::NONE;
    pend_.pic = nullptr;
    pend_.headers.clear();
    pend_.slices.clear();
    pend_.last_independent = nullptr;
    dpb_.reset();
    if (clear_parameter_sets) ps_mgr_.reset();
    cvs_id_ = 0;
    // §8.1 random access: re-arm wait-for-IRAP and treat the next CRA as the
    // first picture of a CVS (NoRaslOutputFlag == 1) — a reset() is a tune-in.
    no_irap_yet_ = true;
    prev_irap_no_rasl_output_ = false;
    handle_cra_as_first_ = true;
    // A reset() is a clean tune-in: clear any pending error-resync state.
    skip_to_next_irap_ = false;
    stats_ = DecodeStats{};
    // §D.3.8: a reset() is a tune-in — drop any recovery-point state so the next
    // stream's recovery_point SEI is reported fresh.
    has_pending_recovery_point_ = false;
    pending_recovery_poc_cnt_ = 0;
    has_recovery_point_ = false;
    recovery_poc_cnt_ = 0;
    recovery_point_poc_ = 0;
    // The per-picture scratch buffers (cu_info_buf_, intra/chroma/motion,
    // cbf/log2/edge grids, sao_params_buf_, sao_backup_, slice_idx_buf_) are
    // intentionally left alone: every one is resize()+fill()'d when a picture
    // starts, so they carry no cross-stream meaning and retaining their
    // capacity is exactly the "reuse the allocated memory" win. The thread
    // pool is persistent infrastructure and must not be torn down.
}

} // namespace hevc
