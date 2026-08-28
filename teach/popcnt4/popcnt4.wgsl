// The bitsliced popcnt4 kernel as a WebGPU compute shader -- the same
// nine gates as popcnt4.c, one bitwise statement per gate, in the same
// order with the same wire names.  Diff the two side by side: the gate
// body is a transliteration, and everything else is calling convention.
//
// Two things change on a GPU:
//
// 1. Lane count.  WGSL's widest integer is u32, so a wire carries 32
//    evaluations per word instead of the C kernel's 64 (the production
//    WebGPU backend, flow/codegen/webgpu/wgsl.py, makes the same
//    choice).  Bit i of every word still belongs to evaluation i.
//
// 2. The call becomes a dispatch.  The C function evaluates one slice
//    per call; here every GPU invocation evaluates one slice, and a
//    single dispatch runs as many invocations as there are slices in
//    the buffer.  The C kernel's parallelism is the width of one word;
//    the GPU's is that width times however many invocations the
//    hardware runs at once -- the statements below do not know or care.
//
// Buffer layout, following the C signature "in[4] and out[3] in BLIF
// port order": slice s reads its four input planes at in_planes[4s+0]
// .. in_planes[4s+3] and writes out_planes[3s+0] .. out_planes[3s+2].

@group(0) @binding(0) var<storage, read> in_planes: array<u32>;
@group(0) @binding(1) var<storage, read_write> out_planes: array<u32>;

@compute @workgroup_size(64)
fn popcnt4(@builtin(global_invocation_id) g: vec3<u32>) {
    let s = g.x;
    if (s >= arrayLength(&in_planes) / 4u) {
        return;
    }

    let x0 = in_planes[4u * s + 0u];
    let x1 = in_planes[4u * s + 1u];
    let x2 = in_planes[4u * s + 2u];
    let x3 = in_planes[4u * s + 3u];

    let t0 = (~x0 & x1) | (x0 & ~x1);
    let t1 = (~t0 & x2) | (t0 & ~x2);
    let y0 = (~t1 & x3) | (t1 & ~x3);
    let t2 = x0 & x1;
    let t3 = t0 & x2;
    let t4 = ~t3 & ~t2;
    let t5 = t1 & x3;
    let y1 = (~t5 & ~t4) | (t5 & t4);
    let y2 = t5 & t2;

    out_planes[3u * s + 0u] = y0;
    out_planes[3u * s + 1u] = y1;
    out_planes[3u * s + 2u] = y2;
}
