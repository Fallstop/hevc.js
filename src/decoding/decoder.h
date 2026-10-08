#pragma once

// Top-level HEVC decoder
// Orchestrates NAL parsing, parameter set management, and frame decoding

#include <cstdint>
#include <cstddef>
#include <deque>
#include <limits>
#include <vector>

#include "common/picture.h"
#include "common/thread_pool.h"
#include "syntax/parameter_sets.h"
#include "syntax/sei.h"
#include "decoding/coding_tree.h"
#include "decoding/dpb.h"

namespace hevc {

enum class DecodeStatus {
    OK,
    NEED_MORE_DATA,
    ERROR,
};

// Running counters since construction / reset(). See hevc_api.h
// (HEVCDecodeStats) for the meaning of each.
struct DecodeStats {
    uint32_t pictures_decoded = 0;
    uint32_t pictures_output = 0;
    uint32_t skipped_wait_irap = 0;
    uint32_t skipped_rasl = 0;
    uint32_t skipped_error = 0;
    uint32_t concealed = 0;
    uint32_t nal_errors = 0;
};

class Decoder {
public:
    Decoder() = default;

    // Decode a complete bitstream (batch mode)
    DecodeStatus decode(const uint8_t* data, size_t size);

    // Feed a chunk of data (incremental mode — one or more complete NAL units)
    // Same as decode() but named for clarity in streaming context.
    DecodeStatus feed(const uint8_t* data, size_t size);

    // feed() with a caller tag: every picture whose first slice segment is in
    // this chunk carries `tag` (Picture::tag) through to output, so the caller
    // can label output frames exactly (e.g. with the sample's timestamp)
    // instead of pairing them with inputs by counting. Untagged feeds use NaN.
    DecodeStatus feed(const uint8_t* data, size_t size, double tag);

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

    // Decode counters (since construction or the last reset()).
    DecodeStats stats() const;

    // Parameter sets (for testing)
    const ParameterSetManager& parameter_sets() const { return ps_mgr_; }

    // Recovery-point SEI state (§D.3.8 — gradual decoding refresh).
    //
    // True once a recovery_point SEI has been seen since the last fresh start /
    // reset(). Cameras using periodic intra-refresh (instead of IDR) signal the
    // first usable output picture this way; a consumer that tuned in mid-stream
    // should treat output before the recovery point as potentially unreliable.
    bool has_recovery_point() const { return has_recovery_point_; }

    // POC of the picture at which output becomes reliable, valid only when
    // has_recovery_point() is true. Derived as the POC of the picture the SEI
    // is associated with plus recovery_poc_cnt (§D.3.8).
    int32_t recovery_point_poc() const { return recovery_point_poc_; }

    // The raw recovery_poc_cnt from the most recent recovery_point SEI.
    int32_t recovery_poc_cnt() const { return recovery_poc_cnt_; }

private:
    // ---- Picture assembly (§7.4.2.4.4/§7.4.2.4.5) ----
    //
    // A picture is assembled slice by slice and may span feed() calls: it is
    // finished when its slices cover the last CTB, when the next picture's
    // first slice (or a slice that cannot belong to it) arrives, at an
    // AUD/EOS/EOB or parameter-set NAL, or at flush().

    // First fields of a slice header, enough to decide which picture a slice
    // segment belongs to without activating parameter sets.
    struct SlicePeek {
        bool first_slice = false;
        bool dependent = false;
        uint32_t pps_id = 0;
        uint32_t address = 0;   // slice_segment_address (0 for the first slice)
        int address_ts = 0;     // the same in tile-scan order
        bool has_poc = false;   // independent non-IDR segment: poc_lsb is valid
        uint32_t poc_lsb = 0;
    };
    bool peek_slice(const NalUnit& nal, SlicePeek& out) const;

    void process_nal(const NalUnit& nal);
    void process_vcl(const NalUnit& nal);
    // Parse the first decodable slice of a picture and set it up (§8.1
    // random-access decisions, POC, RPS, output process, allocation).
    void start_picture(const NalUnit& nal, const SlicePeek& peek);
    // Decode one slice segment of the pending picture (`parsed`: its header,
    // when the caller already parsed it).
    void decode_slice(const NalUnit& nal, const SlicePeek& peek, SliceHeader* parsed);
    void decode_slice_data(const NalUnit& nal, SliceHeader& sh, int slice_number);
    // Complete the pending picture: coverage check, in-loop filters, reference
    // marking, output marking. Drops it if it cannot be completed.
    void finish_picture();
    // Drop the pending picture (decode error); arms skip-to-next-IRAP.
    void fail_picture();
    // Absorb the remaining slices of a picture that is not decoded.
    void begin_skip(const SlicePeek& peek, NalUnitType nut);

    enum class PendingMode { NONE, DECODING, SKIPPING };
    struct PendingPicture {
        PendingMode mode = PendingMode::NONE;
        // Grouping keys of the picture the next slices must match
        NalUnitType nut = NalUnitType::TRAIL_N;
        uint32_t pps_id = 0;
        bool has_poc = false;
        uint32_t poc_lsb = 0;
        int next_ctb_ts = 0;        // tile-scan address just past the last segment
        // DECODING only
        Picture* pic = nullptr;
        const SPS* sps = nullptr;
        const PPS* pps = nullptr;
        std::deque<SliceHeader> headers;          // every parsed segment (stable)
        std::vector<const SliceHeader*> slices;   // independent header per slice number
        const SliceHeader* last_independent = nullptr;  // nullptr: current slice lost
        bool prev_segment_ok = false;  // the segment a dependent one continues
        bool error = false;            // some slice data failed to decode
        bool pic_output_flag = true;
        DecodingContext ctx;
        CabacEngine cabac;
    };
    PendingPicture pend_;

    // Snapshot the reference lists just built for slice `slice_number`.
    void record_slice_refs(int slice_number);
    ParameterSetManager ps_mgr_;
    DPB dpb_;
    DPB::OutputLimits output_limits_;
    DecodeStats stats_;
    // Tag of the feed() call in progress
    double feed_tag_ = std::numeric_limits<double>::quiet_NaN();

    // Output pictures (accumulated across all decoded pictures)
    std::vector<Picture*> output_pics_;

    // CVS counter — incremented at each IRAP with NoRaslOutputFlag
    int32_t cvs_id_ = 0;

    // §8.1 random-access state (camera mid-GOP tune-in).
    //
    // no_irap_yet_: true until the first IRAP is decoded after a fresh start or
    // reset(). While set, all non-IRAP VCL pictures are skipped (they reference
    // pictures we never decoded) — "wait for a random access point".
    //
    // prev_irap_no_rasl_output_: the NoRaslOutputFlag (§8.1) of the most recent
    // IRAP in decoding order. RASL pictures associated with an IRAP whose
    // NoRaslOutputFlag == 1 are undecodable (their references were discarded)
    // and are skipped.
    //
    // handle_cra_as_first_: true at the start of the bitstream and after an
    // EOS_NUT or reset(), so the next CRA is treated as the first picture of a
    // CVS and gets NoRaslOutputFlag == 1.
    bool no_irap_yet_ = true;
    bool prev_irap_no_rasl_output_ = false;
    bool handle_cra_as_first_ = true;

    // Error-resync state (F4 — robustness on 24/7 lossy camera feeds).
    //
    // skip_to_next_irap_: set when a picture fails to decode (bad/truncated VCL
    // payload, parse error, or a thrown bitstream over-read). While set, every
    // non-IRAP VCL picture is dropped — its references may be the corrupted
    // picture or pictures that depended on it — and the decoder resynchronizes at
    // the next IRAP, where a clean CVS restarts. Mirrors the §8.1 wait-for-random
    // -access-point behaviour (no_irap_yet_) but is re-armed by a decode failure
    // rather than a fresh start/reset, so the whole feed() buffer is never lost.
    bool skip_to_next_irap_ = false;

    // §D.3.8 recovery-point SEI state.
    //
    // pending_recovery_poc_cnt_ holds recovery_poc_cnt from a PREFIX_SEI that has
    // been parsed but not yet bound to its associated picture (the SEI precedes
    // the picture in decoding order); it is consumed by the next decoded picture
    // to resolve recovery_point_poc_ = picture POC + recovery_poc_cnt.
    bool has_pending_recovery_point_ = false;
    int32_t pending_recovery_poc_cnt_ = 0;
    bool has_recovery_point_ = false;
    int32_t recovery_poc_cnt_ = 0;
    int32_t recovery_point_poc_ = 0;

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

    // Phase 9: SAO backup buffers (reused across frames to avoid per-frame
    // allocation). Byte-sized so they hold either uint8 (native 8-bit) or
    // uint16 plane bytes; apply_sao reinterprets per the plane Sample type.
    std::vector<uint8_t> sao_backup_[3];

    // Phase 10: slice number per CTU, and which CTUs were decoded
    std::vector<uint16_t> slice_idx_buf_;
    std::vector<uint8_t> ctb_decoded_buf_;

    // Phase 9B: persistent thread pool for WPP parallel decode
    ThreadPool thread_pool_;
};

} // namespace hevc
