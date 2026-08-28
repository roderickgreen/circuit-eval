// The libraries under demo/src get their WGSL from the bundler, because in
// the browser that is what they do: each kernel is emitted as a
// content-hashed asset and fetched by that name. Off the page there is no
// bundler and no server, so the harnesses read the .wgsl off disk instead.
//
// Import this for its side effect, before anything loads a kernel. The
// default is the committed set the page runs (demo/src/kernels/); the
// Makefile's validate-kernels target points KERNELS at the staging dir
// (flow/codegen/webgpu/build/kernels/) instead, so a fresh generation is vetted *before* it is
// copied over the committed one.
import { setKernelReader } from "../../demo/src/lib/gpu.ts";

// queried, not just read: asking for an ungranted env var would prompt or
// throw, and the default must work under a bare --allow-read
const canEnv = Deno.permissions.querySync(
  { name: "env", variable: "KERNELS" }).state === "granted";
const dir = (canEnv && Deno.env.get("KERNELS"))
  || new URL("../../demo/src/kernels", import.meta.url).pathname;

setKernelReader((name) => Deno.readTextFile(`${dir}/${name}.wgsl`));
