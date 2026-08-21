import { fileURLToPath, URL } from 'node:url';
import vue from '@vitejs/plugin-vue';
import { defineConfig } from 'vitest/config';
// Frontend logic tests. The record parser itself is tested in Rust (cargo test), because the
// frontend never parses serial text; see contracts/app-ipc.md.
export default defineConfig({
    plugins: [vue()],
    resolve: {
        alias: {
            '@': fileURLToPath(new URL('./src', import.meta.url)),
        },
    },
    test: {
        environment: 'jsdom',
        include: ['src/**/*.spec.ts'],
        globals: true,
    },
});
