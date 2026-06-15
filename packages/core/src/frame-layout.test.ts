import { describe, it, expect } from "vitest";
import { FRAME_STRUCT, copyPlane, readFrameStruct, type FrameHeap } from "./frame-layout.js";

/**
 * Tests for the single-source-of-truth WASM frame-layout module. These guard
 * the native `HEVCFrame` struct ABI (size + field offsets) and the shared
 * `copyPlane`/`readFrameStruct` helpers that decoder.ts and worker.ts both
 * consume, so any drift in the C++ struct or the dedup'd copy path is caught
 * by a unit test rather than at runtime.
 */

describe("HEVCFrame struct ABI guard", () => {
  it("pins the total struct size", () => {
    // The C++ side writes a 48-byte struct (used for _malloc on the JS side).
    expect(FRAME_STRUCT.SIZE).toBe(48);
  });

  it("pins every field offset (catches native struct drift)", () => {
    // i32 pointers + i32 scalars laid out contiguously. If the C++ struct
    // reorders or resizes a field, this fails loudly instead of silently
    // misreading geometry/pointers.
    expect(FRAME_STRUCT.Y_PTR).toBe(0);
    expect(FRAME_STRUCT.CB_PTR).toBe(4);
    expect(FRAME_STRUCT.CR_PTR).toBe(8);
    expect(FRAME_STRUCT.WIDTH).toBe(12);
    expect(FRAME_STRUCT.HEIGHT).toBe(16);
    expect(FRAME_STRUCT.STRIDE_Y).toBe(20);
    expect(FRAME_STRUCT.STRIDE_C).toBe(24);
    expect(FRAME_STRUCT.CHROMA_WIDTH).toBe(28);
    expect(FRAME_STRUCT.CHROMA_HEIGHT).toBe(32);
    expect(FRAME_STRUCT.BIT_DEPTH).toBe(36);
    expect(FRAME_STRUCT.POC).toBe(40);
    expect(FRAME_STRUCT.BYTES_PER_SAMPLE).toBe(44);
  });

  it("every field fits within the declared struct size", () => {
    // The last field (4-byte i32 at offset 44) must end exactly at SIZE.
    const offsets = Object.entries(FRAME_STRUCT)
      .filter(([k]) => k !== "SIZE")
      .map(([, v]) => v);
    const lastFieldEnd = Math.max(...offsets) + 4;
    expect(lastFieldEnd).toBe(FRAME_STRUCT.SIZE);
    for (const off of offsets) {
      expect(off).toBeGreaterThanOrEqual(0);
      expect(off + 4).toBeLessThanOrEqual(FRAME_STRUCT.SIZE);
    }
  });
});

describe("readFrameStruct", () => {
  it("reads each field from its ABI offset relative to framePtr", () => {
    const framePtr = 5000;
    const values: Record<number, number> = {
      [framePtr + FRAME_STRUCT.Y_PTR]: 100,
      [framePtr + FRAME_STRUCT.CB_PTR]: 200,
      [framePtr + FRAME_STRUCT.CR_PTR]: 300,
      [framePtr + FRAME_STRUCT.WIDTH]: 1920,
      [framePtr + FRAME_STRUCT.HEIGHT]: 1080,
      [framePtr + FRAME_STRUCT.STRIDE_Y]: 1920,
      [framePtr + FRAME_STRUCT.STRIDE_C]: 960,
      [framePtr + FRAME_STRUCT.CHROMA_WIDTH]: 960,
      [framePtr + FRAME_STRUCT.CHROMA_HEIGHT]: 540,
      [framePtr + FRAME_STRUCT.BIT_DEPTH]: 8,
      [framePtr + FRAME_STRUCT.POC]: 7,
      [framePtr + FRAME_STRUCT.BYTES_PER_SAMPLE]: 1,
    };
    const m: FrameHeap = {
      getValue: (ptr: number) => values[ptr]!,
      HEAPU8: new Uint8Array(0),
      HEAPU16: new Uint16Array(0),
    };
    const s = readFrameStruct(m, framePtr);
    expect(s).toEqual({
      yPtr: 100, cbPtr: 200, crPtr: 300,
      width: 1920, height: 1080,
      strideY: 1920, strideC: 960,
      chromaWidth: 960, chromaHeight: 540,
      bitDepth: 8, poc: 7,
      bytesPerSample: 1,
    });
  });

  it("normalizes bytesPerSample: 2 for any value other than 1", () => {
    const base: Record<number, number> = {
      [FRAME_STRUCT.Y_PTR]: 0, [FRAME_STRUCT.CB_PTR]: 0, [FRAME_STRUCT.CR_PTR]: 0,
      [FRAME_STRUCT.WIDTH]: 0, [FRAME_STRUCT.HEIGHT]: 0,
      [FRAME_STRUCT.STRIDE_Y]: 0, [FRAME_STRUCT.STRIDE_C]: 0,
      [FRAME_STRUCT.CHROMA_WIDTH]: 0, [FRAME_STRUCT.CHROMA_HEIGHT]: 0,
      [FRAME_STRUCT.BIT_DEPTH]: 0, [FRAME_STRUCT.POC]: 0,
    };
    const make = (bps: number): FrameHeap => ({
      getValue: (ptr: number) =>
        ptr === FRAME_STRUCT.BYTES_PER_SAMPLE ? bps : base[ptr]!,
      HEAPU8: new Uint8Array(0),
      HEAPU16: new Uint16Array(0),
    });
    expect(readFrameStruct(make(1), 0).bytesPerSample).toBe(1);
    expect(readFrameStruct(make(2), 0).bytesPerSample).toBe(2);
    // Defensive: an unexpected width is treated as the 16-bit path.
    expect(readFrameStruct(make(0), 0).bytesPerSample).toBe(2);
  });
});

describe("copyPlane", () => {
  function heapOnly(parts: Partial<FrameHeap>): FrameHeap {
    return {
      getValue: () => 0,
      HEAPU8: new Uint8Array(0),
      HEAPU16: new Uint16Array(0),
      ...parts,
    };
  }

  it("bytesPerSample=1: copies a strided plane out of HEAPU8 into a packed Uint8Array", () => {
    // 2x2 plane, stride 4 — columns 2,3 of each row are padding to be skipped.
    const HEAPU8 = new Uint8Array(64);
    const ptr = 8;
    HEAPU8[ptr + 0] = 10; HEAPU8[ptr + 1] = 11;            // row 0 visible
    HEAPU8[ptr + 2] = 0xff; HEAPU8[ptr + 3] = 0xff;        // row 0 padding
    HEAPU8[ptr + 4] = 12; HEAPU8[ptr + 5] = 13;            // row 1 visible (stride 4)
    HEAPU8[ptr + 6] = 0xff; HEAPU8[ptr + 7] = 0xff;        // row 1 padding

    const out = copyPlane(heapOnly({ HEAPU8 }), ptr, 2, 2, 4, 1);
    expect(out).toBeInstanceOf(Uint8Array);
    expect(out.length).toBe(4); // packed: width*height, stride removed
    expect(Array.from(out)).toEqual([10, 11, 12, 13]);

    // It is a copy, not a view: mutating the heap does not change the output.
    HEAPU8[ptr] = 99;
    expect(out[0]).toBe(10);
  });

  it("bytesPerSample=2: copies a strided plane out of HEAPU16 (base = ptr >> 1)", () => {
    // Sample base is ptr >> 1 for the 16-bit path. ptr=16 -> sample base 8.
    const HEAPU16 = new Uint16Array(64);
    const ptr = 16;
    const base = ptr >> 1; // 8
    HEAPU16[base + 0] = 1000; HEAPU16[base + 1] = 1001;    // row 0 visible
    HEAPU16[base + 2] = 0xdead;                            // row 0 padding (stride 3)
    HEAPU16[base + 3] = 1002; HEAPU16[base + 4] = 1003;    // row 1 visible (stride 3)
    HEAPU16[base + 5] = 0xdead;                            // row 1 padding

    const out = copyPlane(heapOnly({ HEAPU16 }), ptr, 2, 2, 3, 2);
    expect(out).toBeInstanceOf(Uint16Array);
    expect(out.length).toBe(4);
    expect(Array.from(out)).toEqual([1000, 1001, 1002, 1003]);

    HEAPU16[base] = 9999;
    expect(out[0]).toBe(1000);
  });

  it("handles stride == width (no padding) for both element widths", () => {
    const HEAPU8 = new Uint8Array([5, 6, 7, 8]);
    const u8 = copyPlane(heapOnly({ HEAPU8 }), 0, 2, 2, 2, 1);
    expect(Array.from(u8)).toEqual([5, 6, 7, 8]);

    const HEAPU16 = new Uint16Array([500, 600, 700, 800]);
    const u16 = copyPlane(heapOnly({ HEAPU16 }), 0, 2, 2, 2, 2);
    expect(Array.from(u16)).toEqual([500, 600, 700, 800]);
  });
});
