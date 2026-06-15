import type { HEVCFrame, HEVCFrameView, HEVCStreamInfo, DecodeResult, DecoderOptions } from "./types.js";
import { FRAME_STRUCT, copyPlane, readFrameStruct } from "./frame-layout.js";

/**
 * Emscripten module interface (subset we use)
 */
interface EmscriptenModule {
  cwrap(name: string, returnType: string | null, argTypes: string[]): Function;
  _malloc(size: number): number;
  _free(ptr: number): void;
  getValue(ptr: number, type: string): number;
  HEAPU8: Uint8Array;
  HEAPU16: Uint16Array;
}

interface DecoderAPI {
  create: () => number;
  destroy: (dec: number) => void;
  decode: (dec: number, ptr: number, size: number) => number;
  getFrameCount: (dec: number) => number;
  getFrame: (dec: number, index: number, framePtr: number) => number;
  getInfo: (dec: number, infoPtr: number) => number;
  feed: (dec: number, ptr: number, size: number) => number;
  drain: (dec: number, countPtr: number) => number;
  getDrainedFrame: (dec: number, index: number, framePtr: number) => number;
  flush: (dec: number) => number;
  reset: (dec: number, clearParameterSets: number) => number;
}

/**
 * HEVC/H.265 Decoder — JavaScript wrapper for the WASM module.
 *
 * @example
 * ```ts
 * const decoder = await HEVCDecoder.create();
 * const { frames, info } = decoder.decode(bitstreamBytes);
 * console.log(`${info.width}x${info.height}, ${frames.length} frames`);
 * decoder.destroy();
 * ```
 */
export class HEVCDecoder {
  private _m: EmscriptenModule;
  private _api: DecoderAPI;
  private _dec: number;
  /**
   * Monotonic generation counter, bumped at the start of every entry point that
   * can mutate, advance, free, or grow the WASM heap (feed/decode/drain/
   * drainViews/flush/reset/destroy). Each {@link HEVCFrameView} captures the
   * generation it was minted at; if the live counter has moved past it, the
   * view's planes may no longer alias the data they were created over.
   */
  private _gen = 0;

  private constructor(module: EmscriptenModule) {
    this._m = module;
    this._api = {
      create: module.cwrap("hevc_decoder_create", "number", []) as () => number,
      destroy: module.cwrap("hevc_decoder_destroy", null, ["number"]) as (dec: number) => void,
      decode: module.cwrap("hevc_decoder_decode", "number", ["number", "number", "number"]) as (dec: number, ptr: number, size: number) => number,
      getFrameCount: module.cwrap("hevc_decoder_get_frame_count", "number", ["number"]) as (dec: number) => number,
      getFrame: module.cwrap("hevc_decoder_get_frame", "number", ["number", "number", "number"]) as (dec: number, index: number, framePtr: number) => number,
      getInfo: module.cwrap("hevc_decoder_get_info", "number", ["number", "number"]) as (dec: number, infoPtr: number) => number,
      feed: module.cwrap("hevc_decoder_feed", "number", ["number", "number", "number"]) as (dec: number, ptr: number, size: number) => number,
      drain: module.cwrap("hevc_decoder_drain", "number", ["number", "number"]) as (dec: number, countPtr: number) => number,
      getDrainedFrame: module.cwrap("hevc_decoder_get_drained_frame", "number", ["number", "number", "number"]) as (dec: number, index: number, framePtr: number) => number,
      flush: module.cwrap("hevc_decoder_flush", "number", ["number"]) as (dec: number) => number,
      reset: module.cwrap("hevc_decoder_reset", "number", ["number", "number"]) as (dec: number, clearParameterSets: number) => number,
    };
    this._dec = this._api.create();
    if (!this._dec) throw new Error("Failed to create HEVC decoder");
  }

  /**
   * Create a new decoder instance. Loads the WASM module.
   */
  static async create(options?: DecoderOptions): Promise<HEVCDecoder> {
    const factoryOpts: Record<string, unknown> = {};
    if (options?.wasmBinaryUrl) {
      factoryOpts.locateFile = () => options.wasmBinaryUrl;
    }

    // 1. Check for globally loaded Emscripten module (via <script> tag)
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const g = globalThis as any;
    if (typeof g.HEVCDecoderModule === "function") {
      const module = await g.HEVCDecoderModule(factoryOpts) as EmscriptenModule;
      return new HEVCDecoder(module);
    }

    // 2. Try dynamic import (works with bundlers like Vite/Webpack)
    const wasmUrl = options?.wasmUrl ?? "./wasm/hevc-decode.js";
    const mod = await import(/* @vite-ignore */ wasmUrl);
    const fn = mod.default ?? mod;
    const module = await (fn(factoryOpts) as Promise<EmscriptenModule>);
    return new HEVCDecoder(module);
  }

  /**
   * Decode a complete HEVC bitstream.
   * @param data Raw .265 bitstream bytes
   */
  decode(data: Uint8Array): DecodeResult {
    this._gen++; // allocation + decode can grow/overwrite the heap
    const m = this._m;
    const ptr = m._malloc(data.length);
    try {
      m.HEAPU8.set(data, ptr);
      const ret = this._api.decode(this._dec, ptr, data.length);
      if (ret !== 0) throw new Error(`Decode failed (code ${ret})`);

      const count = this._api.getFrameCount(this._dec);
      const frames: HEVCFrame[] = [];
      for (let i = 0; i < count; i++) {
        const frame = this._extractFrame(i);
        if (frame) frames.push(frame);
      }

      const info = this._extractInfo();
      return { frames, info };
    } finally {
      m._free(ptr);
    }
  }

  /** Number of decoded frames available */
  get frameCount(): number {
    return this._api.getFrameCount(this._dec);
  }

  /** Get stream info (available after decode) */
  get info(): HEVCStreamInfo | null {
    return this._extractInfo();
  }

  private _extractFrame(index: number): HEVCFrame | null {
    const m = this._m;
    const framePtr = m._malloc(FRAME_STRUCT.SIZE);
    try {
      const ret = this._api.getFrame(this._dec, index, framePtr);
      if (ret !== 0) return null;
      return this._readFrameFromPtr(framePtr);
    } finally {
      m._free(framePtr);
    }
  }

  private _extractDrainedFrame(index: number): HEVCFrame | null {
    const m = this._m;
    const framePtr = m._malloc(FRAME_STRUCT.SIZE);
    try {
      const ret = this._api.getDrainedFrame(this._dec, index, framePtr);
      if (ret !== 0) return null;
      return this._readFrameFromPtr(framePtr);
    } finally {
      m._free(framePtr);
    }
  }

  private _readFrameFromPtr(framePtr: number): HEVCFrame {
    const m = this._m;
    const s = readFrameStruct(m, framePtr);
    // Copy the planes out of the heap; the discriminated `bytesPerSample`
    // pins the element type for the whole frame so no narrowing assert is
    // needed downstream.
    if (s.bytesPerSample === 1) {
      return {
        bytesPerSample: 1,
        y:  copyPlane(m, s.yPtr,  s.width, s.height, s.strideY, 1) as Uint8Array,
        cb: copyPlane(m, s.cbPtr, s.chromaWidth, s.chromaHeight, s.strideC, 1) as Uint8Array,
        cr: copyPlane(m, s.crPtr, s.chromaWidth, s.chromaHeight, s.strideC, 1) as Uint8Array,
        width: s.width, height: s.height,
        chromaWidth: s.chromaWidth, chromaHeight: s.chromaHeight,
        bitDepth: s.bitDepth, poc: s.poc,
      };
    }
    return {
      bytesPerSample: 2,
      y:  copyPlane(m, s.yPtr,  s.width, s.height, s.strideY, 2) as Uint16Array,
      cb: copyPlane(m, s.cbPtr, s.chromaWidth, s.chromaHeight, s.strideC, 2) as Uint16Array,
      cr: copyPlane(m, s.crPtr, s.chromaWidth, s.chromaHeight, s.strideC, 2) as Uint16Array,
      width: s.width, height: s.height,
      chromaWidth: s.chromaWidth, chromaHeight: s.chromaHeight,
      bitDepth: s.bitDepth, poc: s.poc,
    };
  }

  private _extractInfo(): HEVCStreamInfo | null {
    const m = this._m;
    const infoPtr = m._malloc(24);
    try {
      const ret = this._api.getInfo(this._dec, infoPtr);
      if (ret !== 0) return null;
      return {
        width:        m.getValue(infoPtr, "i32"),
        height:       m.getValue(infoPtr + 4, "i32"),
        bitDepth:     m.getValue(infoPtr + 8, "i32"),
        chromaFormat: m.getValue(infoPtr + 12, "i32"),
        profile:      m.getValue(infoPtr + 16, "i32"),
        level:        m.getValue(infoPtr + 20, "i32"),
      };
    } finally {
      m._free(infoPtr);
    }
  }

  // --- Incremental API (streaming) ---

  /**
   * Feed a chunk of data containing one or more complete NAL units.
   * The decoder accumulates parameter sets and decodes pictures incrementally.
   * Call drain() after each feed() to retrieve output-ready frames.
   */
  feed(data: Uint8Array): void {
    this._gen++; // allocation + decode-in-feed can grow/overwrite the heap
    const m = this._m;
    const ptr = m._malloc(data.length);
    try {
      m.HEAPU8.set(data, ptr);
      const ret = this._api.feed(this._dec, ptr, data.length);
      if (ret !== 0) throw new Error(`Feed failed (code ${ret})`);
    } finally {
      m._free(ptr);
    }
  }

  /**
   * Drain output-ready frames from the decoder (§C.5.2 bumping process).
   * Returns frames in display order, only when ready per DPB constraints.
   * Frames are valid until the next feed() or destroy() call.
   */
  drain(): HEVCFrame[] {
    this._gen++; // advances the DPB and may overwrite the heap
    const m = this._m;
    const countPtr = m._malloc(4);
    try {
      const ret = this._api.drain(this._dec, countPtr);
      if (ret !== 0) return [];
      const count = m.getValue(countPtr, "i32");
      const frames: HEVCFrame[] = [];
      for (let i = 0; i < count; i++) {
        const frame = this._extractDrainedFrame(i);
        if (frame) frames.push(frame);
      }
      return frames;
    } finally {
      m._free(countPtr);
    }
  }

  /**
   * Zero-copy variant of {@link drain}. Returns frames whose planes are
   * sub-array views directly into the WASM heap — no per-frame allocation or
   * copy. Use this in the same thread/worker that consumes the pixels (e.g. a
   * transcode or render worker) to remove the steady-state plane copy.
   *
   * LIFETIME — the returned views (and all their planes) alias the live WASM
   * heap and are **invalidated by the next decoder call** on this decoder:
   * `decode()`, `feed()`, `drain()`, `drainViews()`, `flush()`, `reset()`, or
   * `destroy()`. Any of these may overwrite, free, or — under
   * `ALLOW_MEMORY_GROWTH` — *detach* the underlying `ArrayBuffer`. They must be
   * consumed **synchronously** before any such call or any other WASM-heap
   * allocation, and must NOT be transferred via `postMessage`, queued, or held
   * across an `await`: the planes do not own an `ArrayBuffer`. Each view carries
   * {@link HEVCFrameView.isValid}/{@link HEVCFrameView.assertValid} so misuse
   * fails loudly (a clear `Error`) instead of silently reading zeros/garbage.
   *
   * If you need to keep pixels past the next decoder call, use the copying
   * {@link drain} instead.
   */
  drainViews(): HEVCFrameView[] {
    this._gen++; // advances the DPB and may overwrite/grow the heap
    const m = this._m;
    const countPtr = m._malloc(4);
    try {
      const ret = this._api.drain(this._dec, countPtr);
      if (ret !== 0) return [];
      const count = m.getValue(countPtr, "i32");
      const views: HEVCFrameView[] = [];
      // All drained pictures stay valid until the next feed/drain, so every
      // view built here is simultaneously live for the caller's loop. The
      // struct scratch is malloc'd once up front (no heap growth occurs in the
      // loop below), so the heap views stay attached while we build them.
      const framePtr = m._malloc(FRAME_STRUCT.SIZE);
      try {
        for (let i = 0; i < count; i++) {
          const r = this._api.getDrainedFrame(this._dec, i, framePtr);
          if (r !== 0) continue;
          views.push(this._readFrameView(framePtr));
        }
      } finally {
        m._free(framePtr);
      }
      return views;
    } finally {
      m._free(countPtr);
    }
  }

  /**
   * Build the lifetime guard pair for a freshly-minted view. The view is valid
   * while (a) the decoder has not advanced its generation past `gen`, and
   * (b) the heap `ArrayBuffer` the planes alias is still the module's live heap.
   *
   * `heap` names the typed array (`HEAPU8` or `HEAPU16`) the view's sub-arrays
   * were carved from: we capture its current `.buffer`, then later re-read the
   * module's *live* heap of the same width and compare. Under
   * `ALLOW_MEMORY_GROWTH` the module swaps `HEAPU8`/`HEAPU16` for fresh typed
   * arrays over a new buffer, so a mismatch means the captured planes now alias
   * a stale/detached buffer.
   */
  private _viewGuard(
    gen: number,
    heap: "HEAPU8" | "HEAPU16",
  ): { isValid(): boolean; assertValid(): void } {
    const self = this;
    const buffer = self._m[heap].buffer;
    const detached = (): boolean => self._m[heap].buffer !== buffer;
    const isValid = (): boolean => self._gen === gen && !detached();
    return {
      isValid,
      assertValid(): void {
        if (self._gen !== gen) {
          throw new Error(
            `HEVCFrameView is stale: a later decoder call (generation ${self._gen} > ${gen}) ` +
            `may have overwritten or freed its heap planes. Consume views synchronously, ` +
            `or use the copying drain() to retain frames.`,
          );
        }
        if (detached()) {
          throw new Error(
            "HEVCFrameView is detached: WASM memory growth replaced the heap ArrayBuffer. " +
            "Its planes now alias a dead buffer. Use the copying drain() to retain frames.",
          );
        }
      },
    };
  }

  private _readFrameView(framePtr: number): HEVCFrameView {
    const m = this._m;
    const s = readFrameStruct(m, framePtr);

    // Sub-array spans from the first visible sample through the last visible
    // sample of the strided plane: (h-1)*stride + w covers every row we read.
    // Spans are in SAMPLES, valid for both element widths.
    const ySpan = s.height > 0 ? s.strideY * (s.height - 1) + s.width : 0;
    const cSpan = s.chromaHeight > 0 ? s.strideC * (s.chromaHeight - 1) + s.chromaWidth : 0;

    if (s.bytesPerSample === 1) {
      // Native 8-bit planes: byte base == sample base, view over HEAPU8.
      // Stamp with the generation + source heap so consumers can detect
      // invalidation (next decoder call / memory growth).
      return {
        ...this._viewGuard(this._gen, "HEAPU8"),
        bytesPerSample: 1,
        y:  m.HEAPU8.subarray(s.yPtr,  s.yPtr  + ySpan),
        cb: m.HEAPU8.subarray(s.cbPtr, s.cbPtr + cSpan),
        cr: m.HEAPU8.subarray(s.crPtr, s.crPtr + cSpan),
        width: s.width, height: s.height,
        chromaWidth: s.chromaWidth, chromaHeight: s.chromaHeight,
        strideY: s.strideY, strideC: s.strideC,
        bitDepth: s.bitDepth, poc: s.poc,
      };
    }
    const yBase = s.yPtr >> 1, cbBase = s.cbPtr >> 1, crBase = s.crPtr >> 1;
    return {
      ...this._viewGuard(this._gen, "HEAPU16"),
      bytesPerSample: 2,
      y:  m.HEAPU16.subarray(yBase,  yBase  + ySpan),
      cb: m.HEAPU16.subarray(cbBase, cbBase + cSpan),
      cr: m.HEAPU16.subarray(crBase, crBase + cSpan),
      width: s.width, height: s.height,
      chromaWidth: s.chromaWidth, chromaHeight: s.chromaHeight,
      strideY: s.strideY, strideC: s.strideC,
      bitDepth: s.bitDepth, poc: s.poc,
    };
  }

  /**
   * Flush all remaining frames from the DPB (call at end of stream).
   * Returns all buffered frames in display order.
   */
  flush(): HEVCFrame[] {
    this._gen++; // drains the DPB and may overwrite the heap
    const ret = this._api.flush(this._dec);
    if (ret !== 0) return [];
    // After flush, drained frames are available via getDrainedFrame
    const m = this._m;
    const countPtr = m._malloc(4);
    try {
      // flush() already populates the drained list, read the count
      // by checking how many frames are available
      const frames: HEVCFrame[] = [];
      const framePtr = m._malloc(FRAME_STRUCT.SIZE);
      try {
        for (let i = 0; ; i++) {
          const r = this._api.getDrainedFrame(this._dec, i, framePtr);
          if (r !== 0) break;
          frames.push(this._readFrameFromPtr(framePtr));
        }
      } finally {
        m._free(framePtr);
      }
      return frames;
    } finally {
      m._free(countPtr);
    }
  }

  /**
   * Reset the decoder so the same instance can decode a new, independent
   * stream — far cheaper than `destroy()` + `create()` because it reuses the
   * WASM instance, its memory, and its internal scratch allocations. Drops the
   * DPB and POC state; clears parameter sets unless `clearParameterSets` is
   * false.
   *
   * Use this between seek/scrub targets, or to recover after a feed failure,
   * instead of tearing down and recreating the decoder. Any frames or views
   * from a prior drain/flush are invalidated by this call — copy out anything
   * you still need first.
   *
   * @param clearParameterSets When true (default) the stored VPS/SPS/PPS are
   *   forgotten, matching per-segment streams that re-supply parameter sets in
   *   each init segment. Pass false only when seeking within a stream whose
   *   parameter sets were delivered once, out-of-band.
   */
  reset(clearParameterSets = true): void {
    if (!this._dec) return;
    this._gen++; // drops DPB/POC state — invalidates any outstanding views
    const ret = this._api.reset(this._dec, clearParameterSets ? 1 : 0);
    if (ret !== 0) throw new Error(`Reset failed (code ${ret})`);
  }

  /** Release decoder resources */
  destroy(): void {
    if (this._dec) {
      this._gen++; // frees the instance — invalidates any outstanding views
      this._api.destroy(this._dec);
      this._dec = 0;
    }
  }
}
