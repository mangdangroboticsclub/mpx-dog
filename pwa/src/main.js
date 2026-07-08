import { mount } from "svelte";
import App from "./app.svelte";
import "./app.css";

const app = mount(App, {
  target: document.getElementById("app"),
});

export default app;

/* ── Service Worker Registration (PWA) ─────────────────────── */
if ("serviceWorker" in navigator) {
  // Register the service worker — wait until load for faster initial render
  window.addEventListener("load", async () => {
    try {
      const registration = await navigator.serviceWorker.register("/sw.js", {
        scope: "/",
      });
      console.log("📡 SW registered:", registration.scope);
    } catch (err) {
      console.warn("⚠️  SW registration failed:", err.message);
    }
  });
}
