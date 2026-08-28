// One solve's command stream: submitting chunks of the grid, reading tally
// snapshots back, and honouring the progress / cancel / pause hooks. The
// grid walks in schedules.ts decide what to submit; this decides how.
//
// With hooks the grid is chunked, each chunk snapshots the monotonic tally,
// and at most MAX_INFLIGHT chunks are queued ahead of the mapAsync drain --
// that keeps the queue fed while bounding both cancel latency and the GPU
// work queued ahead of anything else sharing the device (a compositor,
// notably). Without hooks only the final submit takes a snapshot.

import type { Kernel } from "./kernels.ts";
import type { RawTally, RunHooks } from "./types.ts";

const MAX_INFLIGHT = 3;
/** GPU work per chunk the paced walks aim for */
export const PACE_MS = 100;
/** cap on the paced chunk size, in deals */
const MAX_PACED_DEALS = 2 ** 38;

const r64 = (t: Uint32Array, i: number) => t[2 * i] + t[2 * i + 1] * 0x100000000;

// decode a tally image (partial or final) into the raw counter object;
// key order is a contract with the validation harness's ref_eval.js
// (JSON-compared there)
export function decodeTally(t: Uint32Array, hilo: boolean): RawTally {
  return hilo
    ? { quarters: r64(t, 0), boards: r64(t, 1), winH: r64(t, 2),
        tieH: r64(t, 3), heroLow: r64(t, 4), splitLow: r64(t, 5),
        noLow: r64(t, 6), scoop: r64(t, 7) }
    : { win: r64(t, 0), tie: r64(t, 1), boards: r64(t, 2) };
}

export class Dispatch {
  readonly hooked: boolean;
  /** largest dispatchWorkgroups(n) the device accepts */
  readonly maxGroups: number;
  /** deals counted so far, per the latest snapshot */
  doneDeals = 0;
  /** set by checkpoint() once the cancel hook fires */
  aborted = false;

  readonly #device: GPUDevice;
  readonly #kernel: Kernel;
  readonly #bind: GPUBindGroup;
  readonly #totalDeals: number;
  readonly #hooks: RunHooks;
  readonly #t0: number;
  #gatedMs = 0;
  #inflight: GPUBuffer[] = [];
  #lastTally: Uint32Array | null = null;

  // zeroes the tally, fires the done=0 tick and starts the clock
  constructor(
    device: GPUDevice, kernel: Kernel, bind: GPUBindGroup, maxGroups: number,
    totalDeals: number, hooks: RunHooks,
  ) {
    this.#device = device;
    this.#kernel = kernel;
    this.#bind = bind;
    this.maxGroups = maxGroups;
    this.#totalDeals = totalDeals;
    this.#hooks = hooks;
    this.hooked = !!(hooks.onProgress || hooks.cancelled || hooks.gate);
    device.queue.writeBuffer(kernel.tally, 0, new Uint32Array(16));
    // tick immediately so the status line shows the run is live before any
    // readback lands
    hooks.onProgress?.({ done: 0, total: totalDeals });
    this.#t0 = performance.now();
  }

  writeUniform(byteOffset: number, words: Uint32Array | number[]): void {
    this.#device.queue.writeBuffer(this.#kernel.uniform, byteOffset,
                                   new Uint32Array(words));
  }

  // Every chunk boundary: park while paused, then answer "give up?". Ordered
  // that way so a stop issued during a pause is seen as soon as the loop
  // wakes. Time parked is held out of the reported ms. Returns (and records)
  // whether the run is now aborted.
  async checkpoint(): Promise<boolean> {
    const { gate, cancelled } = this.#hooks;
    const g = gate?.();
    if (g) {
      const p0 = performance.now();
      await g;
      this.#gatedMs += performance.now() - p0;
    }
    if (cancelled?.()) this.aborted = true;
    return this.aborted;
  }

  // one dispatch of nGroups workgroups; with snapshot, a copy of the tally
  // is queued behind it for drain() to read back
  submit(nGroups: number, snapshot: boolean): void {
    const enc = this.#device.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(this.#kernel.pipeline);
    pass.setBindGroup(0, this.#bind);
    pass.dispatchWorkgroups(nGroups);
    pass.end();
    if (snapshot) {
      const staging = this.#device.createBuffer({
        size: 64,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
      });
      enc.copyBufferToBuffer(this.#kernel.tally, 0, staging, 0, 64);
      this.#inflight.push(staging);
    }
    this.#device.queue.submit([enc.finish()]);
  }

  // drain one snapshot once MAX_INFLIGHT are queued
  async throttle(): Promise<void> {
    if (this.#inflight.length >= MAX_INFLIGHT) await this.#drain();
  }

  // deals worth ~paceMs of GPU work at the rate measured so far, or null
  // before the first snapshot has landed
  pacedDeals(paceMs: number): number | null {
    if (this.doneDeals === 0) return null;
    return Math.min(MAX_PACED_DEALS,
                    (this.doneDeals / Math.max(1, this.#elapsedMs())) * paceMs);
  }

  // wait for every queued snapshot; ms is GPU time with parked time held out
  async finish(): Promise<{ tally: Uint32Array | null; ms: number }> {
    while (this.#inflight.length) await this.#drain();
    return { tally: this.#lastTally, ms: this.#elapsedMs() };
  }

  // time since the first dispatch, with time parked in the gate held out
  #elapsedMs(): number {
    return performance.now() - this.#t0 - this.#gatedMs;
  }

  async #drain(): Promise<void> {
    const staging = this.#inflight.shift()!;
    await staging.mapAsync(GPUMapMode.READ);
    const tally = new Uint32Array(staging.getMappedRange().slice(0));
    staging.unmap();
    staging.destroy();
    this.#lastTally = tally;
    const raw = decodeTally(tally, this.#kernel.hilo);
    this.doneDeals = raw.boards;
    // counters are monotonic, so raw is the exact partial tally at this
    // snapshot and callers can show live win/tie/loss while running
    this.#hooks.onProgress?.({ done: this.doneDeals, total: this.#totalDeals, raw });
  }
}
