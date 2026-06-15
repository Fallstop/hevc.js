# @hevcjs/dashjs-plugin

## 1.1.1

### Patch Changes

- Updated dependencies [[`081e846`](https://github.com/privaloops/hevc.js/commit/081e846d299a8458f2fa2d9b7cd2329f6b279594), [`a2020e1`](https://github.com/privaloops/hevc.js/commit/a2020e15028b0eab9639a035c671eae0e2006317), [`98505c5`](https://github.com/privaloops/hevc.js/commit/98505c52eda5f705870115bbd7dc5a983ee836dc), [`bc90311`](https://github.com/privaloops/hevc.js/commit/bc90311db2ae0d54a584e8a64f2fdf1afcd96e7b), [`953bb21`](https://github.com/privaloops/hevc.js/commit/953bb2127bf2fdc1a4f6d9d7174ae20c8c2e2b45), [`cc97762`](https://github.com/privaloops/hevc.js/commit/cc9776240e379e9046d7ec194b6a4800f97c137d), [`8964893`](https://github.com/privaloops/hevc.js/commit/896489339a992a56d0b9443c38fade364d3902d6)]:
  - @hevcjs/core@1.4.0

## 1.1.0

### Minor Changes

- [#137](https://github.com/privaloops/hevc.js/pull/137) [`6c2bf32`](https://github.com/privaloops/hevc.js/commit/6c2bf32c7ae704a9f341d95a407bb58313dee955) Thanks [@privaloops](https://github.com/privaloops)! - Add compute-aware ABR feedback for the Shaka and dash.js plugins. **On by default.**

  Mainstream player ABR algorithms pick a variant from network bandwidth
  alone — in a normal pipeline, fetch + parse + MSE append is essentially
  free compared to the network. With these plugins we add a real
  client-side cost: WASM HEVC decode + WebCodecs H.264 encode. A variant
  can be perfectly reachable from a bandwidth standpoint and still saturate
  the device's transcode budget, draining the buffer without the ABR
  algorithm ever noticing.

  This release adds a player-agnostic decider in `@hevcjs/core`
  (`ComputeAwareDecider`) plus a perf bus (`subscribeSegmentStat`) the
  transcoder publishes to after each segment. Both plugins ship an adapter
  that subscribes to the bus and narrows the variants the host ABR is
  allowed to choose from — via Shaka's
  `player.configure({ abr: { restrictions } })` and dash.js's
  `player.updateSettings({ streaming: { abr: { maxBitrate } } })`. The
  host ABR controller is never replaced.

  Usage (Shaka — needs an extra `attachComputeAware(player)` because the
  player doesn't exist at registration time):

  ```js
  // On by default.
  const handle = registerHevcTransmuxer(shaka, { wasmUrl, workerUrl });
  const player = new shaka.Player();
  handle.attachComputeAware(player);
  // To tune: { adaptiveCompute: { targetSpeedX: 1.5, lowerAfter: 1 } }
  // To opt out: { adaptiveCompute: false }
  ```

  Usage (dash.js — wires directly, the player is already available):

  ```js
  // On by default.
  await attachHevcSupport(player, { wasmUrl, workerUrl });
  // To tune:    { adaptiveCompute: { targetSpeedX: 1.5 } }
  // To opt out: { adaptiveCompute: false }
  ```

  The Shaka handle remains callable for backwards compatibility
  (`handle()` still unregisters the transmuxer), with `unregister()` /
  `attachComputeAware(player)` exposed as methods on the same handle.

  Both plugins now also re-export `subscribeSegmentStat` and
  `SegmentPerfStat` from `@hevcjs/core` so consumers can plug their own
  telemetry on the perf bus without a separate `@hevcjs/core` dependency.

  Closes [#127](https://github.com/privaloops/hevc.js/issues/127).

### Patch Changes

- Updated dependencies [[`6c2bf32`](https://github.com/privaloops/hevc.js/commit/6c2bf32c7ae704a9f341d95a407bb58313dee955)]:
  - @hevcjs/core@1.3.0

## 1.0.6

### Patch Changes

- Updated dependencies [[`186b4ce`](https://github.com/privaloops/hevc.js/commit/186b4ce3fa54c347c1aa1d8e5ddf5ca86a5098b1)]:
  - @hevcjs/core@1.2.1

## 1.0.5

### Patch Changes

- Updated dependencies [[`e96f66b`](https://github.com/privaloops/hevc.js/commit/e96f66b48a167ff3ddd9a4cb53885c7dad34c1f6), [`0e4bb47`](https://github.com/privaloops/hevc.js/commit/0e4bb47fd91c05dcce08e53bd4235d2b7fd31c63)]:
  - @hevcjs/core@1.2.0
