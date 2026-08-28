// WebGPU device + pipeline helpers shared by the benchmark and the equity
// kernels.

export interface GPUContext {
  adapter: GPUAdapter;
  device: GPUDevice;
}

// cache holds the promise, not the resolved device: callers overlap during
// the two acquisition awaits, and resources are not portable between devices
let cached: Promise<GPUContext> | null = null;

export function getGPU(): Promise<GPUContext> {
  if (!cached) {
    cached = acquireGPU();
    cached.catch(() => { cached = null; }); // don't cache failures
  }
  return cached;
}

async function acquireGPU(): Promise<GPUContext> {
  if (!navigator.gpu) {
    throw new Error("WebGPU unavailable — use a recent Chrome/Edge/Safari over localhost or https");
  }
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("no WebGPU adapter found");
  const need = ["maxBufferSize", "maxStorageBufferBindingSize",
                "maxComputeWorkgroupsPerDimension"] as const;
  const requiredLimits: Record<string, number> = {};
  for (const k of need) requiredLimits[k] = adapter.limits[k];
  const device = await adapter.requestDevice({ requiredLimits });
  device.addEventListener("uncapturederror", (e) =>
    console.error("WebGPU:", (e as GPUUncapturedErrorEvent).error.message));
  return { adapter, device };
}

// "" when the adapter reports nothing identifying. The three fields overlap
// (Safari repeats one word in all of them; elsewhere "apple" sits beside
// "apple m5"), so only tokens no other token contains are kept: equal ones
// resolve to the first, nested ones to the longer.
export function adapterLabel(adapter: GPUAdapter): string {
  const i = (adapter.info ?? {}) as Partial<GPUAdapterInfo>;
  const parts = [i.vendor, i.architecture, i.description]
    .map((s) => (s ?? "").trim())
    .filter(Boolean);
  return parts
    .filter((s, n) => !parts.some((o, m) =>
      m !== n && o.toLowerCase().includes(s.toLowerCase())
      && (o.length > s.length || m < n)))
    .join(" · ");
}

export async function makePipeline(
  device: GPUDevice, src: string, entryPoint: string,
): Promise<GPUComputePipeline> {
  device.pushErrorScope("validation");
  const module = device.createShaderModule({ code: src });
  const info = await module.getCompilationInfo();
  for (const m of info.messages) {
    if (m.type === "error") console.error(`shader @${m.lineNum}:${m.linePos}: ${m.message}`);
  }
  const err = await device.popErrorScope();
  if (err) throw new Error("shader validation failed: " + err.message);
  // async creation keeps compilation off the main thread
  return device.createComputePipelineAsync({ layout: "auto", compute: { module, entryPoint } });
}

// The value kernels' meta line (wgsl.py). nmask is how many card masks a
// hand takes -- 1 for hold'em, 2 for omaha's hole + board -- and lanes is
// the word width, so one invocation owns 32 * lanes hands.
export function parseMeta(src: string) {
  const m = src.match(/\/\/ meta: nin=(\d+) nout=(\d+) nmask=(\d+) lanes=(\d+)/);
  if (!m) throw new Error("no meta line in shader");
  return { nin: +m[1], nout: +m[2], nmask: +m[3], lanes: +m[4] };
}

// The WGSL in src/kernels is pulled in as a build asset, so each kernel is
// emitted under a content-hashed name and can be served immutable. Vite
// rewrites the glob into a literal name -> URL map at build time (the
// pattern must be a literal). Under base "./" each URL resolves against the
// bundle's own location, so a Pages project subpath needs no config.
let assets: Record<string, string> | undefined;

function kernelURL(name: string): string {
  assets ??= import.meta.glob("../kernels/*.wgsl", {
    query: "?url", import: "default", eager: true,
  }) as Record<string, string>;
  const url = assets[`../kernels/${name}.wgsl`];
  if (!url) throw new Error(`no kernel named ${name}`);
  return url;
}

// The deno harnesses have no bundler; they install a disk reader here before
// importing anything that loads a kernel
// (verify/webgpu/kernel_fetch.js).
type KernelReader = (name: string) => Promise<string>;
let reader: KernelReader | null = null;
export const setKernelReader = (fn: KernelReader) => { reader = fn; };

export async function loadKernelSource(name: string): Promise<string> {
  if (reader) return reader(name);
  const url = kernelURL(name);
  const r = await fetch(url);
  if (!r.ok) throw new Error(`fetch ${url}: HTTP ${r.status}`);
  return r.text();
}

// A mapped readback is the only reliable fence: onSubmittedWorkDone can
// resolve before the work retires on some stacks (seen on deno/wgpu).
export async function fence(device: GPUDevice, buf: GPUBuffer) {
  const staging = device.createBuffer({
    size: 16,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
  });
  const enc = device.createCommandEncoder();
  enc.copyBufferToBuffer(buf, 0, staging, 0, 16);
  device.queue.submit([enc.finish()]);
  await staging.mapAsync(GPUMapMode.READ);
  staging.unmap();
  staging.destroy();
}
