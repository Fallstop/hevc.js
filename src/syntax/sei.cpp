#include "syntax/sei.h"
#include "bitstream/bitstream_reader.h"
#include "common/debug.h"

namespace hevc {

namespace {

// §7.3.5 — read a payloadType / payloadSize value as a sum of bytes, where each
// 0xFF byte contributes 255 and the loop ends on the first byte < 0xFF (which is
// added too). Used for both ff_byte accumulators in sei_message().
uint32_t read_sei_extensible(BitstreamReader& bs) {
    uint32_t value = 0;
    uint32_t byte;
    do {
        byte = bs.read_bits(8);
        value += byte;
    } while (byte == 0xFF && !bs.eof());
    return value;
}

// §D.3.8 — recovery_point( payloadSize )
void parse_recovery_point(BitstreamReader& bs, RecoveryPointSei& out) {
    out.recovery_poc_cnt = bs.read_se();
    out.exact_match_flag = bs.read_flag();
    out.broken_link_flag = bs.read_flag();
}

// §D.3.7 — user_data_unregistered( payloadSize )
void parse_user_data_unregistered(BitstreamReader& bs, uint32_t payload_size,
                                  UserDataUnregisteredSei& out) {
    // 16-byte uuid_iso_iec_11578, then (payloadSize - 16) data bytes.
    uint32_t n = (payload_size < 16) ? payload_size : 16;
    for (uint32_t i = 0; i < n; i++)
        out.uuid_iso_iec_11578[i] = static_cast<uint8_t>(bs.read_bits(8));
    out.data.clear();
    for (uint32_t i = 16; i < payload_size; i++)
        out.data.push_back(static_cast<uint8_t>(bs.read_bits(8)));
}

} // namespace

// §7.3.5 — sei_rbsp(): one or more sei_message().
SeiMessages parse_sei(BitstreamReader& bs) {
    SeiMessages msgs;

    // do { sei_message() } while (more_rbsp_data())
    do {
        uint32_t payload_type = read_sei_extensible(bs);
        uint32_t payload_size = read_sei_extensible(bs);

        // Guard against a corrupt payloadSize running past the buffer.
        if (payload_size > bs.bits_remaining() / 8) {
            HEVC_LOG(PARSE, "SEI: payloadType=%u payloadSize=%u exceeds buffer; stopping",
                     payload_type, payload_size);
            break;
        }

        size_t payload_start = bs.byte_position();

        switch (static_cast<SeiPayloadType>(payload_type)) {
        case SeiPayloadType::RECOVERY_POINT:
            parse_recovery_point(bs, msgs.recovery_point);
            msgs.has_recovery_point = true;
            HEVC_LOG(PARSE, "SEI recovery_point: poc_cnt=%d exact=%d broken_link=%d",
                     msgs.recovery_point.recovery_poc_cnt,
                     msgs.recovery_point.exact_match_flag,
                     msgs.recovery_point.broken_link_flag);
            break;
        case SeiPayloadType::USER_DATA_UNREGISTERED:
            parse_user_data_unregistered(bs, payload_size, msgs.user_data_unregistered);
            msgs.has_user_data_unregistered = true;
            HEVC_LOG(PARSE, "SEI user_data_unregistered: %u data bytes",
                     payload_size > 16 ? payload_size - 16 : 0);
            break;
        default:
            HEVC_LOG(PARSE, "SEI: skipping unknown payloadType=%u (size=%u)",
                     payload_type, payload_size);
            break;
        }

        // Always realign to the message boundary: handlers may not consume the
        // full payload (e.g. recovery_point leaves payload_extension/alignment
        // bits) and unknown types are skipped wholesale. Seeking to the next
        // message keeps the loop in sync regardless.
        bs.seek_to_byte(payload_start + payload_size);
    } while (!bs.eof() && bs.more_rbsp_data());

    return msgs;
}

} // namespace hevc
