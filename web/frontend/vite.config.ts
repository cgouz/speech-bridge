import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// Built assets are embedded into sb-server via go:embed (see ../web.go).
// base: './' keeps every asset URL relative so the SPA works when served
// from any mount path.
export default defineConfig({
  plugins: [vue()],
  base: './',
  build: {
    outDir: '../dist',
    emptyOutDir: true,
  },
  server: {
    proxy: {
      '/v1/stream': { target: 'ws://127.0.0.1:8080', ws: true },
      '/v1': 'http://127.0.0.1:8080',
      '/health': 'http://127.0.0.1:8080',
      '/ready': 'http://127.0.0.1:8080',
      '/metrics': 'http://127.0.0.1:8080',
    },
  },
})
