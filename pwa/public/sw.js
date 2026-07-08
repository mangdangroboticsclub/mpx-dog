/* MPX-Dog Cache-First Service Worker
 *
 * Strategy:
 *   - On first visit: cache all static assets (install event).
 *   - On subsequent visits: serve from cache instantly (fetch event).
 *   - API calls (/v1/*) and WebSocket: network-only (bypass cache).
 *   - SPA navigation: always serve index.html for any navigation request.
 */

const CACHE = "mpx-dog-v2";

const PRECACHE_URLS = [
  "/",
  "/index.html",
  "/m.js",
  "/index.css",
  "/icon.svg",
  "/manifest.json",
  "/sw.js",
];

/* ── Install: populate cache with all static assets ── */
self.addEventListener("install", (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(CACHE);
      // Warm up the cache — individual addAll calls are more resilient
      // than one big batch if a single resource fails.
      for (const url of PRECACHE_URLS) {
        try {
          await cache.add(url);
        } catch {
          console.warn("⚠️  Failed to precache", url);
        }
      }
    })(),
  );
  self.skipWaiting();
});

/* ── Activate: clean old caches ── */
self.addEventListener("activate", (event) => {
  event.waitUntil(
    (async () => {
      const keys = await caches.keys();
      await Promise.all(
        keys
          .filter((k) => k !== CACHE)
          .map((k) => {
            console.log("🧹 Cleaning old cache:", k);
            return caches.delete(k);
          }),
      );
    })(),
  );
  self.clients.claim();
});

/* ── Fetch: cache-first for assets, network-only for API/WSS ── */
self.addEventListener("fetch", (event) => {
  const { request } = event;
  const url = new URL(request.url);

  // Bypass cache for API calls and WebSocket upgrades
  if (url.pathname.startsWith("/v1/") || request.url.startsWith("ws://")) {
    return;
  }

  // SPA: serve index.html for all navigation requests
  if (request.mode === "navigate") {
    event.respondWith(
      (async () => {
        try {
          const response = await fetch(request);
          const cache = await caches.open(CACHE);
          cache.put("/index.html", response.clone());
          return response;
        } catch {
          const fallback = await caches.match("/index.html");
          if (fallback) return fallback;
          return new Response("Offline", { status: 503 });
        }
      })(),
    );
    return;
  }

  // Cache-first for static assets
  event.respondWith(
    (async () => {
      const cached = await caches.match(request);
      if (cached) return cached;

      try {
        const response = await fetch(request);
        if (response.ok) {
          const cache = await caches.open(CACHE);
          cache.put(request, response.clone());
        }
        return response;
      } catch {
        return new Response("Offline", { status: 503 });
      }
    })(),
  );
});
