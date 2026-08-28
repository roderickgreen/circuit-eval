/// <reference types="vitest/config" />
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  // relative asset URLs, so the built folder drops onto any static host --
  // including a GitHub Pages project subpath -- with no rebuild
  base: "./",
  server: { port: 8000, strictPort: true },
  preview: { port: 8000, strictPort: true },
  build: {
    target: "es2022",
  },
  test: {
    // node, not jsdom: these are the app-logic tests, and half the engine
    // cases assert what happens when navigator.gpu is absent -- which node
    // provides natively. Anything that needs a real GPU lives in
    // ../verify/webgpu/ instead.
    environment: "node",
    include: ["test/**/*.test.ts"],
  },
});
