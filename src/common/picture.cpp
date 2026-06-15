#include "common/picture.h"
#include <cstdio>
#include <cstring>

namespace hevc {

void Picture::allocate(int w, int h, ChromaFormat fmt, int bd_luma, int bd_chroma) {
    pic_width_in_luma = w;
    pic_height_in_luma = h;
    bit_depth_luma = bd_luma;
    bit_depth_chroma = bd_chroma;
    chroma_format = fmt;

    int sub_w = SubWidthC(fmt);
    int sub_h = SubHeightC(fmt);

    // Luma plane
    width[0]  = w;
    height[0] = h;
    stride[0] = w;

    // Chroma planes
    if (fmt == ChromaFormat::MONOCHROME) {
        width[1] = width[2] = 0;
        height[1] = height[2] = 0;
        stride[1] = stride[2] = 0;
    } else {
        width[1]  = width[2]  = w / sub_w;
        height[1] = height[2] = h / sub_h;
        stride[1] = stride[2] = w / sub_w;
    }

    // Storage width per sample: native uint8 (1 byte) when every plane is 8-bit
    // (≈all security-camera HEVC) to halve memory traffic through MC / deblock /
    // SAO and free a future 2× SIMD lane width; uint16 (2 bytes) for >8-bit.
    // The pixel kernels are templated on the plane Sample type and dispatch on
    // this field, so the decoded output is bit-identical either way.
    bytes_per_sample = (bd_luma <= 8 && bd_chroma <= 8) ? 1 : 2;

    for (int c = 0; c < 3; c++) {
        if (width[c] > 0 && height[c] > 0) {
            plane_bytes[c].assign(
                static_cast<size_t>(stride[c]) * height[c] * bytes_per_sample, 0);
        } else {
            plane_bytes[c].clear();
        }
    }
}

bool Picture::write_yuv(const char* path) const {
    FILE* fp = fopen(path, "wb");
    if (!fp) return false;

    // Determine output bit depth per plane
    int bd[3] = { bit_depth_luma, bit_depth_chroma, bit_depth_chroma };

    // Conformance window crop offsets per plane (spec §7.4.3.2.1)
    // Luma offsets are in luma samples, chroma scaled by SubWidthC/SubHeightC
    int sub_w = SubWidthC(chroma_format);
    int sub_h = SubHeightC(chroma_format);

    int crop_left[3]   = { conf_win_left, conf_win_left / sub_w, conf_win_left / sub_w };
    int crop_right[3]  = { conf_win_right, conf_win_right / sub_w, conf_win_right / sub_w };
    int crop_top[3]    = { conf_win_top, conf_win_top / sub_h, conf_win_top / sub_h };
    int crop_bottom[3] = { conf_win_bottom, conf_win_bottom / sub_h, conf_win_bottom / sub_h };

    // Reusable row buffer for 8-bit conversion (avoid per-line allocation)
    std::vector<uint8_t> row_buf;

    for (int c = 0; c < 3; c++) {
        if (width[c] == 0 || height[c] == 0) continue;

        int out_width  = width[c] - crop_left[c] - crop_right[c];
        int out_height = height[c] - crop_top[c] - crop_bottom[c];

        if (bd[c] <= 8) {
            row_buf.resize(out_width);
        }

        for (int y = crop_top[c]; y < crop_top[c] + out_height; y++) {
            if (bd[c] <= 8) {
                // Write as 8-bit. Read native storage directly: uint8 planes need
                // no conversion; uint16 planes take the low byte. Both yield the
                // identical byte because the decoder clips 8-bit samples to [0,255].
                if (bytes_per_sample == 1) {
                    const uint8_t* row = plane_ptr<uint8_t>(c) + y * stride[c];
                    for (int x = 0; x < out_width; x++) {
                        row_buf[x] = row[crop_left[c] + x];
                    }
                } else {
                    const uint16_t* row = plane_ptr<uint16_t>(c) + y * stride[c];
                    for (int x = 0; x < out_width; x++) {
                        row_buf[x] = static_cast<uint8_t>(row[crop_left[c] + x]);
                    }
                }
                if (fwrite(row_buf.data(), 1, out_width, fp) !=
                    static_cast<size_t>(out_width)) {
                    fclose(fp);
                    return false;
                }
            } else {
                // Write as 16-bit little-endian (>8-bit content is always uint16 storage).
                const uint16_t* row = plane_ptr<uint16_t>(c) + y * stride[c];
                const uint16_t* start = row + crop_left[c];
                if (fwrite(start, sizeof(uint16_t), out_width, fp) !=
                    static_cast<size_t>(out_width)) {
                    fclose(fp);
                    return false;
                }
            }
        }
    }

    fclose(fp);
    return true;
}

} // namespace hevc
