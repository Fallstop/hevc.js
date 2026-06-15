// HEVC Decoder C API implementation

#include "wasm/hevc_api.h"
#include "decoding/decoder.h"
#include "common/types.h"

#include <cstring>
#include <vector>

struct HEVCDecoder {
    hevc::Decoder decoder;
    std::vector<hevc::Picture*> output;   // batch mode
    std::vector<hevc::Picture*> drained;  // incremental mode
    const hevc::SPS* last_sps = nullptr;
};

extern "C" {

HEVCDecoder* hevc_decoder_create(void) {
    return new (std::nothrow) HEVCDecoder();
}

void hevc_decoder_destroy(HEVCDecoder* dec) {
    delete dec;
}

int hevc_decoder_decode(HEVCDecoder* dec, const uint8_t* data, size_t size) {
    if (!dec || !data || size == 0) return HEVC_ERROR;

    try {
        auto status = dec->decoder.decode(data, size);
        if (status != hevc::DecodeStatus::OK) return HEVC_ERROR;

        dec->output = dec->decoder.output_pictures();
        return HEVC_OK;
    } catch (...) {
        // Catch C++ exceptions (e.g. BitstreamReader read past end on multi-slice)
        // Still return partial results if any frames were decoded
        dec->output = dec->decoder.output_pictures();
        return dec->output.empty() ? HEVC_ERROR : HEVC_OK;
    }
}

int hevc_decoder_get_frame_count(HEVCDecoder* dec) {
    if (!dec) return 0;
    return static_cast<int>(dec->output.size());
}

int hevc_decoder_get_frame(HEVCDecoder* dec, int index, HEVCFrame* frame) {
    if (!dec || !frame || index < 0 || index >= static_cast<int>(dec->output.size()))
        return HEVC_ERROR;

    const auto* pic = dec->output[index];
    int sub_w = hevc::SubWidthC(pic->chroma_format);
    int sub_h = hevc::SubHeightC(pic->chroma_format);

    // Cropped dimensions
    int crop_w = pic->pic_width_in_luma - pic->conf_win_left - pic->conf_win_right;
    int crop_h = pic->pic_height_in_luma - pic->conf_win_top - pic->conf_win_bottom;

    // Plane pointers offset by conformance window
    int y_offset = pic->conf_win_top * pic->stride[0] + pic->conf_win_left;
    int c_offset = (pic->conf_win_top / sub_h) * pic->stride[1] +
                   (pic->conf_win_left / sub_w);

    // Offsets are in SAMPLES, so they add correctly to a uint8_t* (1 byte/sample)
    // or a uint16_t* (2 bytes/sample). For uint8 storage the pointer is reinterpreted
    // to fit the field type; the consumer re-bases per bytes_per_sample.
    if (pic->bytes_per_sample == 1) {
        frame->y  = reinterpret_cast<const uint16_t*>(pic->plane_ptr<uint8_t>(0) + y_offset);
        frame->cb = reinterpret_cast<const uint16_t*>(pic->plane_ptr<uint8_t>(1) + c_offset);
        frame->cr = reinterpret_cast<const uint16_t*>(pic->plane_ptr<uint8_t>(2) + c_offset);
    } else {
        frame->y  = pic->plane_ptr<uint16_t>(0) + y_offset;
        frame->cb = pic->plane_ptr<uint16_t>(1) + c_offset;
        frame->cr = pic->plane_ptr<uint16_t>(2) + c_offset;
    }
    frame->width = crop_w;
    frame->height = crop_h;
    frame->stride_y = pic->stride[0];
    frame->stride_c = pic->stride[1];
    frame->chroma_width = crop_w / sub_w;
    frame->chroma_height = crop_h / sub_h;
    frame->bit_depth = pic->bit_depth_luma;
    frame->poc = pic->poc;
    frame->bytes_per_sample = pic->bytes_per_sample;

    return HEVC_OK;
}

// --- Incremental API ---

int hevc_decoder_feed(HEVCDecoder* dec, const uint8_t* data, size_t size) {
    if (!dec || !data || size == 0) return HEVC_ERROR;

    try {
        auto status = dec->decoder.feed(data, size);
        return (status == hevc::DecodeStatus::OK) ? HEVC_OK : HEVC_ERROR;
    } catch (...) {
        return HEVC_ERROR;
    }
}

int hevc_decoder_drain(HEVCDecoder* dec, int* count) {
    if (!dec || !count) return HEVC_ERROR;

    try {
        dec->drained = dec->decoder.drain();
        *count = static_cast<int>(dec->drained.size());
        return HEVC_OK;
    } catch (...) {
        *count = 0;
        return HEVC_ERROR;
    }
}

int hevc_decoder_get_drained_frame(HEVCDecoder* dec, int index, HEVCFrame* frame) {
    if (!dec || !frame || index < 0 || index >= static_cast<int>(dec->drained.size()))
        return HEVC_ERROR;

    const auto* pic = dec->drained[index];
    int sub_w = hevc::SubWidthC(pic->chroma_format);
    int sub_h = hevc::SubHeightC(pic->chroma_format);

    int crop_w = pic->pic_width_in_luma - pic->conf_win_left - pic->conf_win_right;
    int crop_h = pic->pic_height_in_luma - pic->conf_win_top - pic->conf_win_bottom;

    int y_offset = pic->conf_win_top * pic->stride[0] + pic->conf_win_left;
    int c_offset = (pic->conf_win_top / sub_h) * pic->stride[1] +
                   (pic->conf_win_left / sub_w);

    if (pic->bytes_per_sample == 1) {
        frame->y  = reinterpret_cast<const uint16_t*>(pic->plane_ptr<uint8_t>(0) + y_offset);
        frame->cb = reinterpret_cast<const uint16_t*>(pic->plane_ptr<uint8_t>(1) + c_offset);
        frame->cr = reinterpret_cast<const uint16_t*>(pic->plane_ptr<uint8_t>(2) + c_offset);
    } else {
        frame->y  = pic->plane_ptr<uint16_t>(0) + y_offset;
        frame->cb = pic->plane_ptr<uint16_t>(1) + c_offset;
        frame->cr = pic->plane_ptr<uint16_t>(2) + c_offset;
    }
    frame->width = crop_w;
    frame->height = crop_h;
    frame->stride_y = pic->stride[0];
    frame->stride_c = pic->stride[1];
    frame->chroma_width = crop_w / sub_w;
    frame->chroma_height = crop_h / sub_h;
    frame->bit_depth = pic->bit_depth_luma;
    frame->poc = pic->poc;
    frame->bytes_per_sample = pic->bytes_per_sample;

    return HEVC_OK;
}

int hevc_decoder_flush(HEVCDecoder* dec) {
    if (!dec) return HEVC_ERROR;

    try {
        dec->drained = dec->decoder.flush();
        return HEVC_OK;
    } catch (...) {
        return HEVC_ERROR;
    }
}

int hevc_decoder_reset(HEVCDecoder* dec, int clear_parameter_sets) {
    if (!dec) return HEVC_ERROR;

    try {
        // Drop the cached frame lists first: they hold raw Picture* into the
        // DPB pool that decoder.reset() is about to free.
        dec->output.clear();
        dec->drained.clear();
        dec->last_sps = nullptr;
        dec->decoder.reset(clear_parameter_sets != 0);
        return HEVC_OK;
    } catch (...) {
        return HEVC_ERROR;
    }
}

int hevc_decoder_get_info(HEVCDecoder* dec, HEVCStreamInfo* info) {
    if (!dec || !info || dec->output.empty()) return HEVC_ERROR;

    const auto* pic = dec->output[0];
    info->width = pic->pic_width_in_luma - pic->conf_win_left - pic->conf_win_right;
    info->height = pic->pic_height_in_luma - pic->conf_win_top - pic->conf_win_bottom;
    info->bit_depth = pic->bit_depth_luma;
    info->chroma_format = static_cast<int>(pic->chroma_format);
    info->profile = 0;
    info->level = 0;

    return HEVC_OK;
}

} // extern "C"
