/**
 * Single source of truth for the WASM `HEVCFrame` struct ABI and the shared
 * plane-copy helper. The C++ side writes a 64-byte struct; the byte offsets and
 * copy logic were previously duplicated across {@link ./decoder.ts} (three read
 * sites) and {@link ./worker.ts}. Keeping them here means the ABI is defined
 * once — change the struct layout in one place when the C++ side changes.
 */

import type { HEVCDecodeStats } from "./types.js";

/** Minimal Emscripten heap surface needed to read the frame struct and planes. */
export interface FrameHeap {
  getValue(ptr: number, type: string): number;
  HEAPU8: Uint8Array;
  HEAPU16: Uint16Array;
}

/**
 * Byte offsets of each field within the 64-byte WASM `HEVCFrame` struct.
 * Pointers (`y`/`cb`/`cr`) are 32-bit, `tag` is an f64 (8-byte aligned), the
 * rest are `i32`; 4 bytes of tail padding round the struct to 64. Must match
 * the C++ struct layout exactly (hevc_api.cpp static_asserts the same).
 */
export const FRAME_STRUCT = {
  /** Total struct size in bytes (used for `_malloc`). */
  SIZE: 64,
  Y_PTR: 0,
  CB_PTR: 4,
  CR_PTR: 8,
  WIDTH: 12,
  HEIGHT: 16,
  STRIDE_Y: 20,
  STRIDE_C: 24,
  CHROMA_WIDTH: 28,
  CHROMA_HEIGHT: 32,
  BIT_DEPTH: 36,
  POC: 40,
  /** Plane storage width in bytes (1 = uint8 planes, 2 = uint16 planes). */
  BYTES_PER_SAMPLE: 44,
  /** f64: the caller's tag from `feed(data, tag)`; NaN when untagged. */
  TAG: 48,
  /** i32: `HEVC_FRAME_*` bit flags. */
  FLAGS: 56,
} as const;

/** Decoded view of the raw WASM frame struct (pointers + geometry). */
export interface FrameStruct {
  yPtr: number;
  cbPtr: number;
  crPtr: number;
  width: number;
  height: number;
  strideY: number;
  strideC: number;
  chromaWidth: number;
  chromaHeight: number;
  bitDepth: number;
  poc: number;
  bytesPerSample: 1 | 2;
  tag: number;
  flags: number;
}

/** Read the 64-byte frame struct at `framePtr` into a typed object. */
export function readFrameStruct(m: FrameHeap, framePtr: number): FrameStruct {
  return {
    yPtr:           m.getValue(framePtr + FRAME_STRUCT.Y_PTR, "*"),
    cbPtr:          m.getValue(framePtr + FRAME_STRUCT.CB_PTR, "*"),
    crPtr:          m.getValue(framePtr + FRAME_STRUCT.CR_PTR, "*"),
    width:          m.getValue(framePtr + FRAME_STRUCT.WIDTH, "i32"),
    height:         m.getValue(framePtr + FRAME_STRUCT.HEIGHT, "i32"),
    strideY:        m.getValue(framePtr + FRAME_STRUCT.STRIDE_Y, "i32"),
    strideC:        m.getValue(framePtr + FRAME_STRUCT.STRIDE_C, "i32"),
    chromaWidth:    m.getValue(framePtr + FRAME_STRUCT.CHROMA_WIDTH, "i32"),
    chromaHeight:   m.getValue(framePtr + FRAME_STRUCT.CHROMA_HEIGHT, "i32"),
    bitDepth:       m.getValue(framePtr + FRAME_STRUCT.BIT_DEPTH, "i32"),
    poc:            m.getValue(framePtr + FRAME_STRUCT.POC, "i32"),
    bytesPerSample: (m.getValue(framePtr + FRAME_STRUCT.BYTES_PER_SAMPLE, "i32") === 1 ? 1 : 2),
    tag:            m.getValue(framePtr + FRAME_STRUCT.TAG, "double"),
    flags:          m.getValue(framePtr + FRAME_STRUCT.FLAGS, "i32"),
  };
}

/**
 * Byte layout of the WASM `HEVCDecodeStats` struct: seven `uint32_t` counters.
 */
export const STATS_STRUCT = {
  SIZE: 28,
  PICTURES_DECODED: 0,
  PICTURES_OUTPUT: 4,
  SKIPPED_WAIT_IRAP: 8,
  SKIPPED_RASL: 12,
  SKIPPED_ERROR: 16,
  CONCEALED: 20,
  NAL_ERRORS: 24,
} as const;

/** Read the `HEVCDecodeStats` struct at `ptr` (counters are unsigned). */
export function readDecodeStats(m: FrameHeap, ptr: number): HEVCDecodeStats {
  const u32 = (off: number) => m.getValue(ptr + off, "i32") >>> 0;
  return {
    picturesDecoded: u32(STATS_STRUCT.PICTURES_DECODED),
    picturesOutput:  u32(STATS_STRUCT.PICTURES_OUTPUT),
    skippedWaitIrap: u32(STATS_STRUCT.SKIPPED_WAIT_IRAP),
    skippedRasl:     u32(STATS_STRUCT.SKIPPED_RASL),
    skippedError:    u32(STATS_STRUCT.SKIPPED_ERROR),
    concealed:       u32(STATS_STRUCT.CONCEALED),
    nalErrors:       u32(STATS_STRUCT.NAL_ERRORS),
  };
}

/**
 * Copy a YUV plane out of the WASM heap into a packed (stride-free) array,
 * handling stride != width. `bytesPerSample` selects the storage width:
 * 1 = uint8 (HEAPU8, byte base == sample base), 2 = uint16 (HEAPU16,
 * base = ptr >> 1).
 */
export function copyPlane(
  m: FrameHeap,
  ptr: number,
  width: number,
  height: number,
  stride: number,
  bytesPerSample: 1 | 2,
): Uint8Array | Uint16Array {
  if (bytesPerSample === 1) {
    const out = new Uint8Array(width * height);
    // Rows are contiguous when nothing is cropped horizontally: one copy.
    if (stride === width) {
      out.set(m.HEAPU8.subarray(ptr, ptr + width * height));
      return out;
    }
    for (let y = 0; y < height; y++) {
      out.set(m.HEAPU8.subarray(ptr + y * stride, ptr + y * stride + width), y * width);
    }
    return out;
  }
  const out = new Uint16Array(width * height);
  const base = ptr >> 1;
  if (stride === width) {
    out.set(m.HEAPU16.subarray(base, base + width * height));
    return out;
  }
  for (let y = 0; y < height; y++) {
    out.set(m.HEAPU16.subarray(base + y * stride, base + y * stride + width), y * width);
  }
  return out;
}
