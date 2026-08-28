// The equity kernel set: which shader a spot runs on, and compiling it.
//
// Every variant/himode has three kernels that walk the same work lists
// (worklist.ts) with a different grid shape:
//
//   dealMajor     one invocation per (board, villain-combo) tile of <= 32
//                 deals; the general path
//   boardMajor    one invocation per board, villain combos in lanes; pays
//                 once the board count fills the GPU (villain unknown,
//                 preflop-sized board space)
//   crossCompare  one workgroup per (board, hero-tile) cell evaluates each
//                 candidate hand once and spends the hero x villain cross
//                 product on a scalar-vs-plane compare; the both-unknown
//                 path, where the fused kernels would re-run the circuit
//                 for every pairing
//
// Cards are ids 4*rank+suit; the kernels work in plane space and the
// planeOfCard meta line from the generator does the mapping here.

import { makePipeline, loadKernelSource } from "../gpu.ts";
import { binom } from "./combinatorics.ts";
import { UNIFORM_BYTES, warmupUniform } from "./uniform.ts";
import type { HiMode, Mask, SpotShape, Variant } from "./types.ts";

export type Route = "dealMajor" | "boardMajor" | "crossCompare";

export interface KernelSpec { file: string; entry: string }

const KERNEL = {
  holdem: { file: "equity_holdem", entry: "main_equity" },
  holdemBM: { file: "equity_holdem_bm", entry: "main_equity_bm" },
  holdemXC: { file: "equity_holdem_xc", entry: "main_equity_xc" },
  omahaHigh: { file: "equity_omaha", entry: "main_equity" },
  omahaHighBM: { file: "equity_omaha_bm", entry: "main_equity_bm" },
  omahaHighXC: { file: "equity_omaha_xc", entry: "main_equity_xc" },
  omahaHilo: { file: "equity_omaha_hilo", entry: "main_equity" },
  omahaHiloBM: { file: "equity_omaha_hilo_bm", entry: "main_equity_bm" },
  omahaHiloXC: { file: "equity_omaha_hilo_xc", entry: "main_equity_xc" },
} satisfies Record<string, KernelSpec>;

// the three kernels a variant/himode can route to
export function kernelSet(variant: Variant, himode?: HiMode): Record<Route, KernelSpec> {
  if (variant === "holdem") {
    return { dealMajor: KERNEL.holdem, boardMajor: KERNEL.holdemBM,
             crossCompare: KERNEL.holdemXC };
  }
  if (himode === "hilo") {
    return { dealMajor: KERNEL.omahaHilo, boardMajor: KERNEL.omahaHiloBM,
             crossCompare: KERNEL.omahaHiloXC };
  }
  return { dealMajor: KERNEL.omahaHigh, boardMajor: KERNEL.omahaHighBM,
           crossCompare: KERNEL.omahaHighXC };
}

// Board-major needs a villain axis to put in lanes; one board = one thread,
// so it only pays once the board count fills the GPU. Postflop spaces are
// ~1k boards, where deal-major is faster, so the split is on board count.
const BOARD_MAJOR_MIN_BOARDS = 32768;

export function chooseRoute(
  forced: Route | null,
  { known, boardLen, heroDraw = 0, villainDraw = 0 }: SpotShape,
): Route {
  const boardMajorAble = villainDraw >= 1;
  if (forced === "boardMajor" && !boardMajorAble) {
    throw new Error("no board-major kernel for this spot");
  }
  if (forced) return forced;
  if (heroDraw >= 1 && villainDraw >= 1) return "crossCompare";
  if (boardMajorAble && binom(52 - known, 5 - boardLen) >= BOARD_MAJOR_MIN_BOARDS) {
    return "boardMajor";
  }
  return "dealMajor";
}

export interface Kernel {
  pipeline: GPUComputePipeline;
  planeOfCard: number[];
  hilo: boolean;
  /** cross-compare kernel: binds a hero list, tiles it `ht` hands at a time */
  xc: boolean;
  ht: number;
  uniform: GPUBuffer;
  /** 8 u64 counters; high uses 3, hi-lo all 8 */
  tally: GPUBuffer;
  bindWith: (boards: GPUBuffer, villains: GPUBuffer, heroes?: GPUBuffer | null) => GPUBindGroup;
}

// cache holds promises, not resolved kernels, so a prewarm racing a solve
// (or two concurrent solves) never compiles the same shader twice
const cache = new Map<string, Promise<Kernel>>();
export function getKernel(device: GPUDevice, spec: KernelSpec): Promise<Kernel> {
  let p = cache.get(spec.file);
  if (!p) {
    p = buildKernel(device, spec);
    cache.set(spec.file, p);
    p.catch(() => cache.delete(spec.file)); // don't cache failures
  }
  return p;
}

async function buildKernel(device: GPUDevice, { file, entry }: KernelSpec): Promise<Kernel> {
  const src = await loadKernelSource(file);
  const planeOfCard = src.match(/\/\/ planeOfCard: ([\d,]+)/)![1].split(",").map(Number);
  const hilo = /meta: equity[^\n]* hilo=1/.test(src);
  const xc = /meta: equity-xc/.test(src);
  const ht = xc ? +src.match(/ ht=(\d+)/)![1] : 0;
  const pipeline = await makePipeline(device, src, entry);

  const uniform = device.createBuffer({
    size: UNIFORM_BYTES,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  // counter i is the u64 pair (word 2i, word 2i+1): the kernels carry into
  // the high word, so tallies never overflow (~10^11-deal spots are fine)
  const tally = device.createBuffer({
    size: 64,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC,
  });
  // the work lists are per-solve (sized by the spot), so bind groups are
  // built per solve too; this placeholder backs the warmup dispatch
  const dummy = device.createBuffer({ size: 8, usage: GPUBufferUsage.STORAGE });
  const bindWith = (
    boardsBuf: GPUBuffer, villainsBuf: GPUBuffer, heroesBuf?: GPUBuffer | null,
  ) => device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0),
    entries: [
      { binding: 0, resource: { buffer: uniform } },
      { binding: 1, resource: { buffer: boardsBuf } },
      { binding: 2, resource: { buffer: villainsBuf } },
      ...(xc ? [{ binding: 3, resource: { buffer: heroesBuf ?? dummy } }] : []),
      { binding: xc ? 4 : 3, resource: { buffer: tally } },
    ],
  });
  // one no-op dispatch (empty work lists hit the early return in every
  // kernel) flushes any driver work deferred past pipeline creation, so the
  // first real solve measures the kernel, not the toolchain. This kernel
  // isn't handed out yet, and later uniform writes are queue-ordered behind
  // this dispatch.
  device.queue.writeBuffer(uniform, 0, warmupUniform(xc));
  const enc = device.createCommandEncoder();
  const pass = enc.beginComputePass();
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindWith(dummy, dummy));
  pass.dispatchWorkgroups(1);
  pass.end();
  device.queue.submit([enc.finish()]);

  return { pipeline, planeOfCard, hilo, xc, ht, uniform, tally, bindWith };
}

export function planeMask(cards: readonly number[], planeOfCard: number[]): Mask {
  let lo = 0, hi = 0;
  for (const c of cards) {
    const p = planeOfCard[c];
    if (p < 32) lo |= 1 << p;
    else hi |= 1 << (p - 32);
  }
  return [lo >>> 0, hi >>> 0];
}
