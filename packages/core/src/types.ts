/**
 * Fields shared by every decoded-frame shape (copied or zero-copy). The plane
 * element type is left to the discriminating subtypes so consumers can narrow
 * on {@link HEVCFrameBase.bytesPerSample} without non-null assertions.
 */
interface HEVCFrameBase {
  /** Luma width (display, after conformance crop) */
  width: number;
  /** Luma height (display) */
  height: number;
  /** Chroma plane width */
  chromaWidth: number;
  /** Chroma plane height */
  chromaHeight: number;
  /** Bit depth (8 or 10) */
  bitDepth: number;
  /** Picture Order Count (display order) */
  poc: number;
}

/**
 * Decoded YUV frame — planes are copied out of the WASM heap (safe to retain
 * and transfer). The plane element type is discriminated by `bytesPerSample`:
 * `1` → `Uint8Array` planes (native 8-bit), `2` → `Uint16Array` planes.
 *
 * Because it is a discriminated union, narrowing on `bytesPerSample` (or using
 * `instanceof Uint8Array`) refines all three planes together — no non-null `!`
 * assertions needed:
 *
 * ```ts
 * if (frame.bytesPerSample === 1) {
 *   const luma: Uint8Array = frame.y; // narrowed
 * }
 * ```
 */
export type HEVCFrame =
  | (HEVCFrameBase & {
      /** Plane storage width in bytes: 1 = `Uint8Array` planes. */
      bytesPerSample: 1;
      /** Luma plane (packed, no stride) */
      y: Uint8Array;
      /** Chroma Cb plane (packed) */
      cb: Uint8Array;
      /** Chroma Cr plane (packed) */
      cr: Uint8Array;
    })
  | (HEVCFrameBase & {
      /** Plane storage width in bytes: 2 = `Uint16Array` planes. */
      bytesPerSample: 2;
      /** Luma plane (packed, no stride) */
      y: Uint16Array;
      /** Chroma Cb plane (packed) */
      cb: Uint16Array;
      /** Chroma Cr plane (packed) */
      cr: Uint16Array;
    });

/**
 * Lifetime/validity surface shared by every {@link HEVCFrameView}. The planes
 * alias the live WASM heap, so a view goes stale the moment the heap is
 * mutated, reallocated, or freed by the next decoder call (see the LIFETIME
 * note on {@link HEVCFrameView}). These let a consumer fail loudly on misuse
 * instead of silently reading zeros/garbage.
 */
interface HEVCFrameViewLifetime {
  /**
   * `true` while the planes still alias the heap region they were created over:
   * the decoder has not advanced its view generation and the heap
   * `ArrayBuffer` has not been detached/replaced by memory growth.
   */
  isValid(): boolean;
  /**
   * Throw a descriptive `Error` if the view is no longer valid (see
   * {@link isValid}). Call this immediately before reading planes if there is
   * any chance a decoder call or WASM allocation happened in between.
   */
  assertValid(): void;
}

/**
 * Zero-copy view into a decoded frame whose planes are sub-arrays directly
 * into the WASM heap (no copy, no allocation). The planes are **strided**:
 * sample (col, row) of luma is `y[row * strideY + col]`, and likewise chroma
 * via `strideC`. Strides may exceed the visible width. The element type is
 * discriminated by `bytesPerSample` exactly as for {@link HEVCFrame}.
 *
 * LIFETIME — read this before using the planes:
 * - The sub-arrays alias the module's WASM heap; they are **invalidated by the
 *   NEXT decoder call** on the same decoder, namely: `decode()`, `feed()`,
 *   `drain()`, `drainViews()`, `flush()`, `reset()`, or `destroy()` — any of
 *   which may overwrite, free, or (under `ALLOW_MEMORY_GROWTH`) *detach* the
 *   underlying `ArrayBuffer`.
 * - They MUST be consumed **synchronously**, before any further decoder call or
 *   WASM-heap allocation. Never retain a view (or its planes) across an
 *   `await`, queue it, or `postMessage`/transfer it — the planes do not own an
 *   `ArrayBuffer` and a transfer would detach the whole module heap.
 * - Memory growth detaches the buffer: after growth the old planes read from a
 *   dead buffer. Call {@link HEVCFrameViewLifetime.assertValid} (or check
 *   {@link HEVCFrameViewLifetime.isValid}) right before reading if a decoder
 *   call/allocation may have intervened.
 *
 * If you need to keep pixels past the next decoder call, use the copying
 * variant {@link HEVCFrame} via `drain()`/`decode()`/`flush()` instead.
 */
export type HEVCFrameView =
  | (HEVCFrameBase & HEVCFrameViewLifetime & {
      /** Plane storage width in bytes: 1 = `Uint8Array` planes. */
      bytesPerSample: 1;
      /** Luma plane view (strided: row r starts at r * strideY). */
      y: Uint8Array;
      /** Chroma Cb plane view (strided by strideC) */
      cb: Uint8Array;
      /** Chroma Cr plane view (strided by strideC) */
      cr: Uint8Array;
      /** Luma row stride in samples (>= width) */
      strideY: number;
      /** Chroma row stride in samples (>= chromaWidth) */
      strideC: number;
    })
  | (HEVCFrameBase & HEVCFrameViewLifetime & {
      /** Plane storage width in bytes: 2 = `Uint16Array` planes. */
      bytesPerSample: 2;
      /** Luma plane view (strided: row r starts at r * strideY). */
      y: Uint16Array;
      /** Chroma Cb plane view (strided by strideC) */
      cb: Uint16Array;
      /** Chroma Cr plane view (strided by strideC) */
      cr: Uint16Array;
      /** Luma row stride in samples (>= width) */
      strideY: number;
      /** Chroma row stride in samples (>= chromaWidth) */
      strideC: number;
    });

/** Stream metadata — available after first decode */
export interface HEVCStreamInfo {
  width: number;
  height: number;
  bitDepth: number;
  /** 0=mono, 1=4:2:0, 2=4:2:2, 3=4:4:4 */
  chromaFormat: number;
  /** Profile IDC (1=Main, 2=Main10) */
  profile: number;
  /** Level IDC (e.g. 93 = Level 3.1) */
  level: number;
}

/** Result of a decode call */
export interface DecodeResult {
  frames: HEVCFrame[];
  info: HEVCStreamInfo | null;
}

/** Options for creating a decoder */
export interface DecoderOptions {
  /** URL to the hevc-decode.js WASM glue file. Auto-resolved if omitted. */
  wasmUrl?: string;
  /** URL to the .wasm binary. Auto-resolved if omitted. */
  wasmBinaryUrl?: string;
}

/** Worker message types (main → worker) */
export type WorkerRequest =
  | { type: "init"; wasmUrl: string }
  | { type: "decode"; data: ArrayBuffer }
  | { type: "feed"; data: ArrayBuffer }
  | { type: "drain" }
  | { type: "flush" }
  | { type: "destroy" };

/** Worker message types (worker → main) */
export type WorkerResponse =
  | { type: "ready" }
  | { type: "info"; info: HEVCStreamInfo }
  | { type: "frame"; index: number; frame: HEVCFrame }
  | { type: "done"; frameCount: number }
  | { type: "drained"; frames: HEVCFrame[] }
  | { type: "flushed"; frames: HEVCFrame[] }
  | { type: "error"; message: string };
