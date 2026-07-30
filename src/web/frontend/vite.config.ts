import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The built bundle lands in ../assets so the embedded C++ web server (civetweb,
// ENABLE_WEB) can serve it as static content. In dev, `npm run dev` proxies the
// live feed + REST endpoints to a running engine on 127.0.0.1:8080.
export default defineConfig({
  plugins: [react()],
  build: {
    outDir: "../assets",
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    proxy: {
      "/stream": { target: "ws://127.0.0.1:8080", ws: true },
      "/api": { target: "http://127.0.0.1:8080", changeOrigin: true },
    },
  },
});
