import { fileURLToPath, URL } from 'node:url'

import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vite'

// Tauri serves the frontend from a fixed port in dev and expects a static build in release.
export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  // Tauri needs a predictable dev server and must fail rather than silently pick another port.
  server: {
    port: 1420,
    strictPort: true,
  },
  // Keep the frontend build free of Tauri's Rust output.
  build: {
    target: 'es2022',
    emptyOutDir: true,
  },
})
