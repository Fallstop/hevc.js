#include "decoding/decoder.h"
#include "bitstream/nal_unit.h"
#include "bitstream/bitstream_reader.h"
#include "syntax/sei.h"
#include "filters/deblocking.h"
#include "filters/sao.h"
#include "common/debug.h"

#include <cstring>
#include <cstdio>

namespace hevc {

DecodeStatus Decoder::decode(const uint8_t* data, size_t size) {
    NalParser parser;
    auto nals = parser.parse(data, size);

    HEVC_LOG(PARSE, "Decoded %zu NAL units", nals.size());

    // Process NALs: parameter sets first, then decode each picture
    size_t i = 0;
    while (i < nals.size()) {
        auto type = nals[i].header.nal_unit_type;

        if (type == NalUnitType::VPS_NUT ||
            type == NalUnitType::SPS_NUT ||
            type == NalUnitType::PPS_NUT) {
            ps_mgr_.process_nal(nals[i]);
            i++;
        } else if (is_vcl(type)) {
            // Collect all VCL NALs belonging to the same picture
            size_t first_vcl = i;
            i++;
            while (i < nals.size() && is_vcl(nals[i].header.nal_unit_type)) {
                // A truncated/empty VCL RBSP would make read_flag() throw past the
                // end. Treat it as a new picture boundary so the corrupt NAL forms
                // its own picture group and is failed cleanly inside the try/catch
                // below — never escaping to abort the whole feed() buffer.
                if (nals[i].rbsp.empty()) break;
                BitstreamReader bs(nals[i].rbsp.data(), nals[i].rbsp.size());
                bool first_slice = bs.read_flag();
                if (first_slice) break; // New picture starts here
                i++;
            }
            size_t vcl_count = i - first_vcl;

            // F4 error resync: a single bad/undecodable picture must NOT abort the
            // whole feed() buffer (24/7 lossy camera feeds). Contain a per-picture
            // failure — including a thrown bitstream over-read on a corrupt slice —
            // record it, drop the half-decoded picture, arm skip-to-next-IRAP, and
            // continue to the next NAL. Output recovers at the next IRAP.
            DecodeStatus status;
            try {
                status = decode_picture(nals, first_vcl, vcl_count);
            } catch (...) {
                status = DecodeStatus::ERROR;
            }
            if (status != DecodeStatus::OK) {
                dpb_.drop_current();
                skip_to_next_irap_ = true;
                HEVC_LOG(PARSE, "Picture decode failed — resyncing at next IRAP%s", "");
            }
        } else if (type == NalUnitType::PREFIX_SEI ||
                   type == NalUnitType::SUFFIX_SEI) {
            // §7.3.5 — parse the SEI RBSP. The recovery_point message (§D.3.8)
            // tells a non-IDR (gradual-refresh) tune-in when output is reliable.
            BitstreamReader bs(nals[i].rbsp.data(), nals[i].rbsp.size());
            SeiMessages sei = parse_sei(bs);
            // recovery_point is a prefix-only message (§D.1): bind it to the next
            // picture in decoding order. A PREFIX_SEI precedes its associated
            // picture, so defer until that picture's POC is known.
            if (sei.has_recovery_point && type == NalUnitType::PREFIX_SEI) {
                has_pending_recovery_point_ = true;
                pending_recovery_poc_cnt_ = sei.recovery_point.recovery_poc_cnt;
            }
            i++;
        } else {
            // §8.1: an end-of-sequence NAL makes the next CRA the first picture
            // of a new CVS (NoRaslOutputFlag == 1, RASL discarded). EOB behaves
            // the same for our purposes.
            if (type == NalUnitType::EOS_NUT || type == NalUnitType::EOB_NUT)
                handle_cra_as_first_ = true;
            // Non-VCL, non-parameter-set NAL (AUD, etc.) — skip
            i++;
        }
    }

    return DecodeStatus::OK;
}

DecodeStatus Decoder::decode_picture(const std::vector<NalUnit>& nals,
                                      size_t first_vcl_idx, size_t vcl_count) {
    const auto& nal = nals[first_vcl_idx];

    // Parse first slice header (always independent)
    SliceHeader first_sh;
    if (!ps_mgr_.parse_slice_header(first_sh, nal)) {
        fprintf(stderr, "Error: failed to parse slice header\n");
        return DecodeStatus::ERROR;
    }

    const SPS* sps = ps_mgr_.active_sps();
    const PPS* pps = ps_mgr_.active_pps();
    if (!sps || !pps) {
        fprintf(stderr, "Error: no active SPS/PPS\n");
        return DecodeStatus::ERROR;
    }

    // §8.1 — random access decisions, evaluated before touching the DPB.
    NalUnitType nut = nal.header.nal_unit_type;
    bool isIRAP = is_irap(nut);
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
        // F4 error resync: after a per-picture decode failure, drop every non-IRAP
        // picture until the next IRAP. Their references may be the corrupted
        // picture (or pictures that depended on it), so decoding them would
        // propagate garbage; recover at the next keyframe.
        if (skip_to_next_irap_) {
            HEVC_LOG(PARSE, "Skipping non-IRAP VCL (resyncing after decode error)%s", "");
            return DecodeStatus::OK;
        }
        // §8.1 wait-for-random-access-point: until the first IRAP after a fresh
        // start/reset, non-IRAP VCL pictures reference frames we never decoded —
        // skip them entirely (don't decode, don't output).
        if (no_irap_yet_) {
            HEVC_LOG(PARSE, "Skipping non-IRAP VCL (waiting for random access point)%s", "");
            return DecodeStatus::OK;
        }
        // §8.1: RASL pictures associated with an IRAP whose NoRaslOutputFlag == 1
        // are undecodable — their references were discarded. Skip them.
        if (is_rasl(nut) && prev_irap_no_rasl_output_) {
            HEVC_LOG(PARSE, "Skipping RASL (associated IRAP has NoRaslOutputFlag=1)%s", "");
            return DecodeStatus::OK;
        }
    }
    bool no_rasl_output_flag = isIRAP && prev_irap_no_rasl_output_;

    HEVC_LOG(PARSE, "Decoding picture: %dx%d type=%d QP=%d slices=%zu",
             sps->pic_width_in_luma_samples, sps->pic_height_in_luma_samples,
             static_cast<int>(first_sh.slice_type), first_sh.SliceQpY, vcl_count);

    // §8.3.1 — POC derivation (from first slice only)
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

    // Allocate picture in DPB
    ChromaFormat fmt = static_cast<ChromaFormat>(sps->chroma_format_idc);
    Picture* pic = dpb_.alloc_picture(
        static_cast<int>(sps->pic_width_in_luma_samples),
        static_cast<int>(sps->pic_height_in_luma_samples),
        fmt, sps->BitDepthY, sps->BitDepthC);
    pic->poc = poc;
    pic->needed_for_output = first_sh.pic_output_flag;

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

    // §8.3.2 — RPS derivation and picture marking
    dpb_.derive_rps(first_sh, *sps, nut, poc, no_rasl_output_flag);

    // §8.3.4 — Reference picture list construction (P and B slices)
    if (first_sh.slice_type != SliceType::I) {
        dpb_.construct_ref_pic_lists(first_sh, *sps, *pps);
        // §8.3.5 — Collocated picture derivation
        dpb_.derive_colpic(first_sh);
    }

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

    // Phase 10: slice index per CTU
    slice_idx_buf_.resize(ctbGridSize);
    std::fill(slice_idx_buf_.begin(), slice_idx_buf_.end(), 0);

    // Setup decoding context (shared across all slices)
    // Note: CabacEngine is re-initialized per-slice via init_contexts + init_decoder
    CabacEngine cabac;
    DecodingContext ctx;
    ctx.wpp_contexts_available = false;
    ctx.thread_pool = &thread_pool_;
    ctx.sps = sps;
    ctx.pps = pps;
    ctx.cabac = &cabac;
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

    // Decode each slice segment — store headers for per-CTU filter parameter lookup
    std::vector<SliceHeader> slice_headers(vcl_count);
    std::vector<const SliceHeader*> slice_header_ptrs(vcl_count);
    int last_independent_idx = 0;
    for (size_t s = 0; s < vcl_count; s++) {
        const auto& slice_nal = nals[first_vcl_idx + s];

        // Parse slice header
        if (s == 0) {
            slice_headers[s] = first_sh;
        } else {
            if (!ps_mgr_.parse_slice_header(slice_headers[s], slice_nal)) {
                fprintf(stderr, "Error: failed to parse slice header for slice %zu\n", s);
                return DecodeStatus::ERROR;
            }
            // §7.4.7.1: dependent slices inherit fields from the independent slice
            if (slice_headers[s].dependent_slice_segment_flag) {
                uint32_t saved_address = slice_headers[s].slice_segment_address;
                bool saved_dependent = slice_headers[s].dependent_slice_segment_flag;
                bool saved_first = slice_headers[s].first_slice_segment_in_pic_flag;
                slice_headers[s] = slice_headers[last_independent_idx];
                slice_headers[s].slice_segment_address = saved_address;
                slice_headers[s].dependent_slice_segment_flag = saved_dependent;
                slice_headers[s].first_slice_segment_in_pic_flag = saved_first;
            } else {
                last_independent_idx = static_cast<int>(s);
            }
        }
        slice_header_ptrs[s] = &slice_headers[s];
    }

    ctx.slice_headers = slice_header_ptrs.data();
    ctx.num_slices = static_cast<int>(vcl_count);

    for (size_t s = 0; s < vcl_count; s++) {
        const auto& slice_nal = nals[first_vcl_idx + s];

        ctx.sh = &slice_headers[s];
        ctx.current_slice_idx = static_cast<int>(s);

        HEVC_LOG(PARSE, "Slice %zu: addr=%d type=%d QP=%d dependent=%d",
                 s, slice_headers[s].slice_segment_address,
                 static_cast<int>(slice_headers[s].slice_type),
                 slice_headers[s].SliceQpY, slice_headers[s].dependent_slice_segment_flag);

        // Create bitstream reader for this slice's RBSP
        BitstreamReader bs(slice_nal.rbsp.data(), slice_nal.rbsp.size());

        // Skip slice header (re-parse to advance the bitstream position)
        {
            SliceHeader sh_skip;
            sh_skip.parse(bs, *sps, *pps, slice_nal.header.nal_unit_type,
                           slice_nal.header.TemporalId());
        }

        // Byte align after slice header
        if (!bs.byte_aligned())
            bs.byte_alignment();
        // Compute slice header coded size for EP byte conversion.
        // The RBSP starts after the 2-byte NAL header. bs.byte_position() is
        // the RBSP offset of slice data start. The coded offset includes EP bytes.
        size_t slice_data_rbsp_pos = bs.byte_position();
        size_t slice_header_coded_size = slice_data_rbsp_pos;
        // Add back EP bytes that fall within the slice header portion
        for (size_t ep : slice_nal.epb_positions) {
            if (ep < slice_header_coded_size + 2) // +2 for NAL header offset in EP positions
                slice_header_coded_size++;
        }

        HEVC_LOG(PARSE, "Slice %zu data starts at bit %zu, remaining=%zu bits",
                 s, bs.bits_read(), bs.bits_remaining());

        // Decode slice data
        if (!decode_slice_segment_data(ctx, bs, slice_nal.epb_positions,
                                        slice_header_coded_size)) {
            fprintf(stderr, "Error: failed to decode slice %zu data\n", s);
            return DecodeStatus::ERROR;
        }
    }

    // §8.7: In-loop filters — deblocking then SAO (once, after all slices)
    ctx.sh = &slice_headers[last_independent_idx];

    apply_deblocking(ctx);
    apply_sao(ctx);

    // Store motion info in the Picture for TMVP access by future frames
    for (int i = 0; i < modeGridW * modeGridH; i++) {
        auto& src = motion_info_buf_[i];
        auto& dst = pic->motion_info_buf[i];
        dst.mv_x[0] = src.mv[0].x; dst.mv_y[0] = src.mv[0].y;
        dst.mv_x[1] = src.mv[1].x; dst.mv_y[1] = src.mv[1].y;
        dst.ref_idx[0] = src.ref_idx[0]; dst.ref_idx[1] = src.ref_idx[1];
        dst.pred_flag[0] = src.pred_flag[0]; dst.pred_flag[1] = src.pred_flag[1];
    }
    // Store ref POC lists for TMVP MV scaling
    pic->ref_poc[0].clear();
    pic->ref_poc[1].clear();
    for (int i = 0; i < dpb_.num_ref_list0(); i++) {
        auto* ref = dpb_.ref_pic_list0(i);
        pic->ref_poc[0].push_back(ref ? ref->poc : 0);
    }
    for (int i = 0; i < dpb_.num_ref_list1(); i++) {
        auto* ref = dpb_.ref_pic_list1(i);
        pic->ref_poc[1].push_back(ref ? ref->poc : 0);
    }

    // §8.1 step 4: mark current picture as short-term reference
    dpb_.mark_current_as_short_term_ref();

    // §C.5.2.3: mark current picture as "needed for output"
    // (PicOutputFlag is 1 for all pictures in Main profile)
    if (dpb_.current_pic()) {
        dpb_.current_pic()->needed_for_output = true;
    }

    return DecodeStatus::OK;
}

DecodeStatus Decoder::feed(const uint8_t* data, size_t size) {
    return decode(data, size);
}

std::vector<Picture*> Decoder::drain() {
    const SPS* sps = ps_mgr_.active_sps();
    if (!sps) return {};
    return dpb_.drain(*sps);
}

std::vector<Picture*> Decoder::flush() {
    return dpb_.flush();
}

std::vector<Picture*> Decoder::output_pictures() {
    // Collect all pictures from the DPB, sort by CVS then POC
    std::vector<Picture*> out;
    for (auto& pic : dpb_.pictures()) {
        out.push_back(pic.get());
    }
    std::sort(out.begin(), out.end(),
              [](const Picture* a, const Picture* b) {
                  if (a->cvs_id != b->cvs_id) return a->cvs_id < b->cvs_id;
                  return a->poc < b->poc;
              });
    return out;
}

void Decoder::reset(bool clear_parameter_sets) {
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
    // §D.3.8: a reset() is a tune-in — drop any recovery-point state so the next
    // stream's recovery_point SEI is reported fresh.
    has_pending_recovery_point_ = false;
    pending_recovery_poc_cnt_ = 0;
    has_recovery_point_ = false;
    recovery_poc_cnt_ = 0;
    recovery_point_poc_ = 0;
    // The per-picture scratch buffers (cu_info_buf_, intra/chroma/motion,
    // cbf/log2/edge grids, sao_params_buf_, sao_backup_, slice_idx_buf_) are
    // intentionally left alone: every one is resize()+fill()'d at the top of
    // decode_picture(), so they carry no cross-stream meaning and retaining
    // their capacity is exactly the "reuse the allocated memory" win. The
    // thread pool is persistent infrastructure and must not be torn down.
}

} // namespace hevc
