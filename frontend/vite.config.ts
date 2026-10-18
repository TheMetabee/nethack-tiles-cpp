import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  // Keep the C++ build's log readable when both run in one terminal.
  clearScreen: false,
  // Debug builds of the app load this URL, so the port is fixed.
  server: {
    port: 1420,
    strictPort: true,
  },
});
