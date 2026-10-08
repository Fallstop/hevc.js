/**
 * Bit flags carried by every decoded frame (`HEVCFrame.flags` /
 * `HEVCFrameView.flags`). Mirrors `HEVC_FRAME_*` in `src/wasm/hevc_api.h`.
 */

/** The picture had decode errors that were concealed. */
export const HEVC_FRAME_CORRUPT = 0x1;
/** The picture was predicted from an unavailable (generated or substituted) reference. */
export const HEVC_FRAME_MISSING_REF = 0x2;
/** The picture was decoded before the stream reached a clean random access point. */
export const HEVC_FRAME_PRE_RECOVERY = 0x4;
