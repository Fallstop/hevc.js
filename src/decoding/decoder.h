#pragma once

// Top-level HEVC decoder
// Orchestrates NAL parsing, parameter set management, and frame decoding

#include <cstdint>
#include <cstddef>
#include <vector>

#include "common/picture.h"
#include "common/thread_pool.h"
#include "syntax/parameter_sets.h"
#include "decoding/coding_tree.h"
#include "decoding/dpb.h"

namespace hevc {

enum class DecodeStatus {
    OK,
    NEED_MORE_DATA,
    ERROR,
};

class Decoder {
public:
    Decoder() = default;

    // Decode a complete bitstream (batch mode)
    DecodeStatus decode(const uint8_t* data, size_t size);

    // Feed a chunk of data (incremental mode — one or more complete NAL units)
    // Same as decode() but named for clarity in streaming context.
    DecodeStatus feed(const uint8_t* data, size_t size);

    // Drain newly output-ready pictures (§C.5.2 bumping process)
    // Returns pictures in display order. Only returns pictures that are
    // ready according to sps_max_num_reorder_pics / DPB size constraints.
    std::vector<Picture*> drain();

    // Flush all remaining pictures from the DPB (end-of-stream)
    // Returns all pictures still marked as "needed for output", in POC order.
    std::vector<Picture*> flush();

    // Get decoded pictures — batch mode (legacy, returns ALL pictures ever decoded)
    std::vector<Picture*> output_pictures();

    // Reset the decoder to its initial state so the SAME instance can decode a
    // new, independent stream — without re-allocating the decoder, its thread
    // pool, or its per-picture scratch buffers (their capacity is retained, so
    // the next stream avoids re-growing them). Drops the DPB and POC state and,
    // when clear_parameter_sets is true (default), the stored VPS/SPS/PPS.
    //
    // Pass clear_parameter_sets=false only when seeking within a stream whose
    // parameter sets were delivered once and are not re-sent — otherwise the
    // next picture would have no active SPS/PPS. For per-segment streams (the
    // @hevcjs MSE/transcode use case) each init segment re-supplies them, so
    // the default is correct.
    //
    // Any Picture*/frame pointer returned by a previous drain()/flush() is
    // invalidated by reset(); drain out every frame you still need first.
    void reset(bool clear_parameter_sets = true);

    // Get DPB (for testing)
    const DPB& dpb() const { return dpb_; }

private:
    DecodeStatus decode_picture(const std::vector<NalUnit>& nals,
                                 size_t first_vcl_idx, size_t vcl_count);

    ParameterSetManager ps_mgr_;
    DPB dpb_;

    // Output pictures (accumulated across all decoded pictures)
    std::vector<Picture*> output_pics_;

    // CVS counter — incremented at each IRAP with NoRaslOutputFlag
    int32_t cvs_id_ = 0;

    // Decoding context allocations (per-picture)
    std::vector<CUInfo> cu_info_buf_;
    std::vector<int> intra_mode_buf_;
    std::vector<int> chroma_mode_buf_;
    std::vector<PUMotionInfo> motion_info_buf_;

    // Phase 6: filter data (per-picture)
    std::vector<uint8_t> cbf_luma_buf_;
    std::vector<uint8_t> log2_tu_size_buf_;
    std::vector<uint8_t> edge_flags_v_buf_;
    std::vector<uint8_t> edge_flags_h_buf_;
    std::vector<DecodingContext::SaoParams> sao_params_buf_;

    // Phase 9: SAO backup buffers (reused across frames to avoid per-frame allocation)
    std::vector<uint16_t> sao_backup_[3];

    // Phase 10: slice index per CTU
    std::vector<uint8_t> slice_idx_buf_;

    // Phase 9B: persistent thread pool for WPP parallel decode
    ThreadPool thread_pool_;
};

} // namespace hevc
