#include "syntax/parameter_sets.h"
#include "common/debug.h"

namespace hevc {

bool ParameterSetManager::process_nal(const NalUnit& nal) {
    BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());

    switch (nal.header.nal_unit_type) {
        case NalUnitType::VPS_NUT: {
            VPS vps;
            if (!vps.parse(bs)) return false;
            int id = vps.vps_video_parameter_set_id;
            if (id > 15) return false;
            vps_[id] = std::move(vps);
            return true;
        }
        case NalUnitType::SPS_NUT: {
            SPS sps;
            if (!sps.parse(bs)) return false;
            int id = static_cast<int>(sps.sps_seq_parameter_set_id);
            if (id > 15) return false;
            sps_[id] = std::move(sps);
            sps_generation_[id] = ++generation_counter_;
            // Derive the PPSs that reference it now, so a bad combination is
            // known early; derive_for_activation() re-checks at activation.
            for (auto& p : pps_) {
                if (p && static_cast<int>(p->pps_seq_parameter_set_id) == id) {
                    if (p->derive(*sps_[id])) p->derived_sps_generation = sps_generation_[id];
                    else p->derived_sps_generation = 0;
                }
            }
            return true;
        }
        case NalUnitType::PPS_NUT: {
            // The PPS syntax does not depend on the SPS; it is parsed alone and
            // derived (tile tables, SPS-dependent ranges) against its SPS when
            // that SPS is available — now, or when a slice activates it. A PPS
            // that arrives before its SPS is therefore kept.
            PPS pps;
            if (!pps.parse_rbsp(bs)) return false;
            int id = static_cast<int>(pps.pps_pic_parameter_set_id);
            int sps_id = static_cast<int>(pps.pps_seq_parameter_set_id);
            if (id > 63 || sps_id > 15) return false;
            if (const SPS* sps = get_sps(sps_id)) {
                // Inconsistent with the SPS in force: keep the previous PPS
                if (!pps.derive(*sps)) return false;
                pps.derived_sps_generation = sps_generation_[sps_id];
            }
            pps_[id] = std::move(pps);
            return true;
        }
        default:
            return false;
    }
}

const PPS* ParameterSetManager::activate(int pps_id) {
    if (pps_id < 0 || pps_id > 63 || !pps_[pps_id]) {
        HEVC_LOG(PARSE, "Slice references PPS %d which is not available", pps_id);
        return nullptr;
    }
    PPS& pps = *pps_[pps_id];
    const int sps_id = static_cast<int>(pps.pps_seq_parameter_set_id);
    const SPS* sps = get_sps(sps_id);
    if (!sps) {
        HEVC_LOG(PARSE, "PPS %d references SPS %d which is not available", pps_id, sps_id);
        return nullptr;
    }
    // Re-derive when the SPS was replaced since (or the PPS was never derived):
    // its tile tables are sized by the SPS picture dimensions.
    if (pps.derived_sps_generation != sps_generation_[sps_id]) {
        if (!pps.derive(*sps)) {
            pps.derived_sps_generation = 0;
            return nullptr;
        }
        pps.derived_sps_generation = sps_generation_[sps_id];
    }
    return &pps;
}

bool ParameterSetManager::parse_slice_header(SliceHeader& sh, const NalUnit& nal) {
    BitstreamReader bs(nal.rbsp.data(), nal.rbsp.size());

    // Peek first_slice_segment_in_pic_flag and slice_pic_parameter_set_id
    bool first_slice = bs.read_flag();

    // Skip no_output_of_prior_pics_flag for IRAP
    if (is_irap(nal.header.nal_unit_type)) {
        bs.read_flag();
    }

    uint32_t pps_id = bs.read_ue();
    if (pps_id > 63) return false;

    const PPS* pps = activate(static_cast<int>(pps_id));
    if (!pps) return false;
    const SPS* sps = get_sps(static_cast<int>(pps->pps_seq_parameter_set_id));

    // Update active parameter set IDs (safe against PS replacement)
    active_sps_id_ = static_cast<int>(pps->pps_seq_parameter_set_id);
    active_pps_id_ = static_cast<int>(pps_id);

    // Re-parse the full slice header from the beginning
    BitstreamReader bs2(nal.rbsp.data(), nal.rbsp.size());
    (void)first_slice;
    return sh.parse(bs2, *sps, *pps, nal.header.nal_unit_type, nal.header.TemporalId());
}

} // namespace hevc
