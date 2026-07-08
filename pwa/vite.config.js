import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

export default defineConfig({
  plugins: [svelte()],

  root: "src",
  publicDir: "../public",
  base: "/",

  build: {
    outDir: "../www",
    emptyOutDir: true,
    minify: "terser",
    terserOptions: {
      compress: {
        drop_console: false,
        drop_debugger: true,
      },
    },
    rollupOptions: {
      output: {
        entryFileNames: "m.js",
        chunkFileNames: "m-[hash].js",
        assetFileNames: (assetInfo) => {
          if (assetInfo.name?.endsWith(".css")) return "index.css";
          if (assetInfo.name?.endsWith(".svg")) return "[name][extname]";
          return "[name][extname]";
        },
      },
    },
  },

  server: {
    host: "0.0.0.0",
    port: 5173,
    strictPort: true,
  },
});
