
export function GpuBanner({ missing }: { missing: boolean }) {
  if (!missing) return null;
  return (
    <div id="gpuBanner">
      This demo requires WebGPU, which this browser does not support.
      Chrome, Edge, Firefox 141+ and Safari 26+ support it.
    </div>
  );
}
