#pragma once

#include <cstdint>
#include <vector>

namespace hevc {

class BitstreamReader;

// SEI payload types — spec Annex D, Table D.1 (subset we recognize)
enum class SeiPayloadType : uint32_t {
    USER_DATA_UNREGISTERED = 5,
    RECOVERY_POINT         = 6,
};

// recovery_point SEI — spec D.2.8 / D.3.8.
// Signals a "gradual decoding refresh" point: from the picture that this SEI is
// associated with, the output becomes usable again recovery_poc_cnt pictures
// later (in output order). Cameras commonly use periodic intra-refresh instead
// of IDR, so this is how a non-IDR random-access tune-in learns when its output
// is reliable.
struct RecoveryPointSei {
    int32_t recovery_poc_cnt = 0;   // se(v)
    bool exact_match_flag = false;   // u(1)
    bool broken_link_flag = false;   // u(1)
};

// user_data_unregistered SEI — spec D.2.7 / D.3.7. Carries vendor-specific data
// keyed by a 16-byte UUID (e.g. x265 stores its build/options string here).
struct UserDataUnregisteredSei {
    uint8_t uuid_iso_iec_11578[16] = {0};
    std::vector<uint8_t> data;       // user_data_payload_byte[]
};

// Aggregated result of parsing a single SEI RBSP (one PREFIX_SEI / SUFFIX_SEI
// NAL may carry several sei_message()s). Only the message types we recognize are
// captured; unknown types are skipped per their payloadSize.
struct SeiMessages {
    bool has_recovery_point = false;
    RecoveryPointSei recovery_point;

    bool has_user_data_unregistered = false;
    UserDataUnregisteredSei user_data_unregistered;
};

// Parse a sei_rbsp() (§7.3.5) — the loop of sei_message()s in one SEI NAL.
// `bs` must be positioned at the first sei_message (i.e. on the SEI RBSP, which
// has the 2-byte NAL header already stripped). Returns the recognized messages.
// Robust to malformed payloads: stops at the RBSP trailing bits / end of data.
SeiMessages parse_sei(BitstreamReader& bs);

} // namespace hevc
