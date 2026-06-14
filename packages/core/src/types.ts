/** Decoded YUV frame — planes are copied out of WASM heap */
export interface HEVCFrame {
  /** Luma plane (packed, no stride) */
  y: Uint16Array;
  /** Chroma Cb plane */
  cb: Uint16Array;
  /** Chroma Cr plane */
  cr: Uint16Array;
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
 * Zero-copy view into a decoded frame whose planes are sub-arrays directly
 * into the WASM heap (no copy, no allocation). The planes are **strided**:
 * sample (col, row) of luma is `y[row * strideY + col]`, and likewise chroma
 * via `strideC`. Strides may exceed the visible width.
 *
 * LIFETIME: the sub-arrays are only valid until the next `feed()`, `drainViews()`,
 * `drain()`, `flush()`, or `destroy()` call on the same decoder — any of which
 * may overwrite or reallocate the underlying heap. Never retain a view (or its
 * planes) past the next decoder call; copy out anything you need to keep.
 */
export interface HEVCFrameView {
  /** Luma plane view (strided: row r starts at r * strideY) */
  y: Uint16Array;
  /** Chroma Cb plane view (strided by strideC) */
  cb: Uint16Array;
  /** Chroma Cr plane view (strided by strideC) */
  cr: Uint16Array;
  /** Luma width (display, after conformance crop) */
  width: number;
  /** Luma height (display) */
  height: number;
  /** Chroma plane width */
  chromaWidth: number;
  /** Chroma plane height */
  chromaHeight: number;
  /** Luma row stride in samples (>= width) */
  strideY: number;
  /** Chroma row stride in samples (>= chromaWidth) */
  strideC: number;
  /** Bit depth (8 or 10) */
  bitDepth: number;
  /** Picture Order Count (display order) */
  poc: number;
}

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
