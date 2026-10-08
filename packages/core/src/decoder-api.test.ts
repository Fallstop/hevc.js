import { describe, it, expect, vi } from "vitest";
import { HEVCDecoder } from "./decoder.js";
import { FRAME_STRUCT, STATS_STRUCT } from "./frame-layout.js";
import {
  HEVC_FRAME_CORRUPT,
  HEVC_FRAME_MISSING_REF,
  HEVC_FRAME_PRE_RECOVERY,
} from "./index.js";

/**
 * The forgiving-decode wrapper API: tagged feed(), per-frame tag/flags, the
 * stats getter and the exported flag constants. WASM is not available under
 * vitest, so the decoder is built via Object.create() over a fake module and
 * API (same pattern as decoder.test.ts).
 */
/* eslint-disable @typescript-eslint/no-explicit-any */

function makeDecoder(module: Record<string, any>, api: Record<string, any>): any {
  const d = Object.create(HEVCDecoder.prototype);
  d._m = module;
  d._api = api;
  d._dec = 7;
  d._gen = 0;
  return d;
}

function heapModule(values: Record<number, number>, ptrs: number[]) {
  let call = 0;
  return {
    _malloc: vi.fn(() => ptrs[call++]!),
    _free: vi.fn(),
    getValue: vi.fn((ptr: number) => values[ptr]),
    HEAPU8: new Uint8Array(4096),
    HEAPU16: new Uint16Array(2048),
  };
}

describe("flag constants", () => {
  it("mirror HEVC_FRAME_* in hevc_api.h", () => {
    expect(HEVC_FRAME_CORRUPT).toBe(1);
    expect(HEVC_FRAME_MISSING_REF).toBe(2);
    expect(HEVC_FRAME_PRE_RECOVERY).toBe(4);
  });
});

describe("HEVCDecoder.feed(data, tag)", () => {
  it("passes the tag to hevc_decoder_feed_tagged", () => {
    const feedTagged = vi.fn().mockReturnValue(0);
    const m = heapModule({}, [64]);
    const d = makeDecoder(m, { feedTagged });
    d.feed(new Uint8Array([0, 0, 1, 0x40]), 90_000.5);
    expect(feedTagged).toHaveBeenCalledWith(7, 64, 4, 90_000.5);
    expect(m._free).toHaveBeenCalledWith(64);
  });

  it("feeds NaN when untagged", () => {
    const feedTagged = vi.fn().mockReturnValue(0);
    const d = makeDecoder(heapModule({}, [64]), { feedTagged });
    d.feed(new Uint8Array([0, 0, 1]));
    expect(Number.isNaN(feedTagged.mock.calls[0]![3])).toBe(true);
  });

  it("throws on a native error and still frees the buffer", () => {
    const feedTagged = vi.fn().mockReturnValue(-1);
    const m = heapModule({}, [64]);
    const d = makeDecoder(m, { feedTagged });
    expect(() => d.feed(new Uint8Array([0]), 1)).toThrow(/Feed failed/);
    expect(m._free).toHaveBeenCalledWith(64);
  });
});

describe("frame tag and flags", () => {
  const framePtr = 2000;
  const frameValues = (tag: number, flags: number, bps: number): Record<number, number> => ({
    1000: 1, // drain count
    [framePtr + FRAME_STRUCT.Y_PTR]: 0,
    [framePtr + FRAME_STRUCT.CB_PTR]: 100,
    [framePtr + FRAME_STRUCT.CR_PTR]: 200,
    [framePtr + FRAME_STRUCT.WIDTH]: 2,
    [framePtr + FRAME_STRUCT.HEIGHT]: 2,
    [framePtr + FRAME_STRUCT.STRIDE_Y]: 2,
    [framePtr + FRAME_STRUCT.STRIDE_C]: 1,
    [framePtr + FRAME_STRUCT.CHROMA_WIDTH]: 1,
    [framePtr + FRAME_STRUCT.CHROMA_HEIGHT]: 1,
    [framePtr + FRAME_STRUCT.BIT_DEPTH]: 8,
    [framePtr + FRAME_STRUCT.POC]: 5,
    [framePtr + FRAME_STRUCT.BYTES_PER_SAMPLE]: bps,
    [framePtr + FRAME_STRUCT.TAG]: tag,
    [framePtr + FRAME_STRUCT.FLAGS]: flags,
  });

  it("drain() frames carry the tag and flags", () => {
    const drain = vi.fn().mockReturnValue(0);
    const getDrainedFrame = vi.fn().mockReturnValue(0);
    const d = makeDecoder(heapModule(frameValues(33_366.7, HEVC_FRAME_CORRUPT, 1), [1000, framePtr]),
                          { drain, getDrainedFrame });
    const frames = d.drain();
    expect(frames).toHaveLength(1);
    expect(frames[0].tag).toBe(33_366.7);
    expect(frames[0].flags).toBe(HEVC_FRAME_CORRUPT);
    expect(frames[0].poc).toBe(5);
  });

  it("drainViews() views carry the tag and flags", () => {
    const drain = vi.fn().mockReturnValue(0);
    const getDrainedFrame = vi.fn().mockReturnValue(0);
    const flags = HEVC_FRAME_MISSING_REF | HEVC_FRAME_PRE_RECOVERY;
    const d = makeDecoder(heapModule(frameValues(Number.NaN, flags, 2), [1000, framePtr]),
                          { drain, getDrainedFrame });
    const views = d.drainViews();
    expect(views).toHaveLength(1);
    expect(Number.isNaN(views[0].tag)).toBe(true);
    expect(views[0].flags).toBe(flags);
    expect(views[0].isValid()).toBe(true);
  });
});

describe("HEVCDecoder.stats", () => {
  it("reads HEVCDecodeStats and frees the scratch struct", () => {
    const ptr = 512;
    const values: Record<number, number> = {
      [ptr + STATS_STRUCT.PICTURES_DECODED]: 12,
      [ptr + STATS_STRUCT.PICTURES_OUTPUT]: 11,
      [ptr + STATS_STRUCT.SKIPPED_WAIT_IRAP]: 2,
      [ptr + STATS_STRUCT.SKIPPED_RASL]: 3,
      [ptr + STATS_STRUCT.SKIPPED_ERROR]: 1,
      [ptr + STATS_STRUCT.CONCEALED]: 4,
      [ptr + STATS_STRUCT.NAL_ERRORS]: 5,
    };
    const getStats = vi.fn().mockReturnValue(0);
    const m = heapModule(values, [ptr]);
    const d = makeDecoder(m, { getStats });
    expect(d.stats).toEqual({
      picturesDecoded: 12,
      picturesOutput: 11,
      skippedWaitIrap: 2,
      skippedRasl: 3,
      skippedError: 1,
      concealed: 4,
      nalErrors: 5,
    });
    expect(getStats).toHaveBeenCalledWith(7, ptr);
    expect(m._free).toHaveBeenCalledWith(ptr);
  });

  it("throws when the native call fails", () => {
    const getStats = vi.fn().mockReturnValue(-1);
    const m = heapModule({}, [512]);
    const d = makeDecoder(m, { getStats });
    expect(() => d.stats).toThrow(/stats/);
    expect(m._free).toHaveBeenCalledWith(512);
  });
});
