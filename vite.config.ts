import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { resolve } from 'path';

export default defineConfig({
  base: '/xiaoling-esp32/',
  resolve: {
    alias: {
      '@': resolve('./src'),
    },
  },
  plugins: [
    vue(),
  ],
  build: {
    chunkSizeWarningLimit: 10 * 1024,
  },
});
