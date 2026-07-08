<script>
  let { navigate } = $props();

  // ── State ──────────────────────────────────────────────────
  let loading = $state(true);
  let error = $state("");

  // AP info
  let apSsid = $state("MPX-Dog");
  let apIp = $state("192.168.2.1");

  // STA info
  let staState = $state("disconnected");
  let staSsid = $state("");
  let staIp = $state("");

  // Connect form
  let inputSsid = $state("");
  let inputPassword = $state("");
  let connecting = $state(false);    // actively sending connect request
  let reconnecting = $state(false);  // waiting for AP to come back after connect
  let connectError = $state("");

  // Background poller for when AP goes down during STA connect
  let pollTimer = $state(null);

  // ── API helpers ────────────────────────────────────────────

  async function fetchStatus() {
    loading = true;
    error = "";
    try {
      const res = await fetch("/v1/wifi/status");
      if (res.ok) {
        const data = await res.json();
        apSsid = data.ap?.ssid ?? "MPX-Dog";
        apIp = data.ap?.ip ?? "192.168.2.1";
        staState = data.sta?.state ?? "disconnected";
        staSsid = data.sta?.ssid ?? "";
        staIp = data.sta?.ip ?? "";

        // If we were waiting for reconnection and now get data, we're back
        if (reconnecting) {
          reconnecting = false;
        }
      } else {
        // Non-ok status means we reached the robot but it errored — that's ok
        if (reconnecting) {
          // Keep waiting, the robot might still be partially up
        } else {
          error = `Status fetch failed (${res.status})`;
        }
      }
    } catch (e) {
      // Network error — robot unreachable
      if (reconnecting) {
        // Expected during WiFi reconfiguration — keep polling silently
      } else {
        error = `Cannot reach robot: ${e.message}`;
      }
    }
    loading = false;
  }

  /**
   * Start polling for robot availability after the AP briefly goes down
   * during STA connect. Calls fetchStatus every 2s until the robot responds
   * with a final state (connected or failed).
   */
  function startReconnectPoller() {
    if (pollTimer) clearTimeout(pollTimer);

    async function poll() {
      try {
        const res = await fetch("/v1/wifi/status");
        if (res.ok) {
          const data = await res.json();
          const state = data.sta?.state ?? "disconnected";

          // Update live state
          staState = state;
          staSsid = data.sta?.ssid ?? staSsid;
          staIp = data.sta?.ip ?? "";

          if (state === "connected" || state === "failed" || state === "disconnected") {
            // Terminal state — stop polling
            reconnecting = false;
            connecting = false;
            return;
          }
          // Still "connecting" — keep polling
        }
        // Non-ok or network error — robot not fully back yet, keep polling
      } catch {
        // Network error — robot not back yet
      }
      // Schedule next poll
      pollTimer = setTimeout(poll, 2000);
    }

    poll();
  }

  async function doConnect() {
    const ssid = inputSsid.trim();
    if (!ssid) return;

    connecting = true;
    reconnecting = false;
    connectError = "";

    let requestSent = false;

    try {
      const res = await fetch("/v1/wifi/connect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid, password: inputPassword }),
      });
      requestSent = true;

      if (res.ok) {
        // Request succeeded — robot accepted. AP may still be up or going down.
        staState = "connecting";
        staSsid = ssid;
        reconnecting = true;
        startReconnectPoller();
      } else {
        const text = await res.text();
        connectError = text || `Connect failed (${res.status})`;
        connecting = false;
      }
    } catch (e) {
      // Network error — expected! The robot stopped its AP to reconfigure.
      // The user's device will disconnect from the MPX-Dog AP briefly.
      // Once the AP restarts, the device should auto-reconnect.
      if (!requestSent) {
        // Request never fully sent — the AP likely went down during the POST.
        // This is normal — the robot is reconfiguring.
        staState = "connecting";
        staSsid = ssid;
        reconnecting = true;
        startReconnectPoller();
      } else {
        connectError = `Connection lost: ${e.message}`;
        connecting = false;
      }
    }
  }

  async function doDisconnect() {
    try {
      await fetch("/v1/wifi/disconnect", { method: "POST" });
      staState = "disconnected";
      staSsid = "";
      staIp = "";
    } catch (e) {
      console.error("Disconnect failed:", e);
    }
  }

  async function doForget() {
    try {
      await fetch("/v1/wifi/forget", { method: "POST" });
      staState = "disconnected";
      staSsid = "";
      staIp = "";
    } catch (e) {
      console.error("Forget failed:", e);
    }
  }

  // ── Helpers ────────────────────────────────────────────────
  function staBadgeClass() {
    switch (staState) {
      case "connected":    return "bg-green-500";
      case "connecting":   return "bg-yellow-500 animate-pulse";
      case "failed":       return "bg-red-500";
      default:             return "bg-gray-500";
    }
  }

  function staLabel() {
    switch (staState) {
      case "connected":    return "Connected";
      case "connecting":   return "Connecting…";
      case "failed":       return "Failed";
      default:             return "Disconnected";
    }
  }

  // ── Init ───────────────────────────────────────────────────
  $effect(() => {
    fetchStatus();
    // Cleanup poller on unmount
    return () => { if (pollTimer) clearTimeout(pollTimer); };
  });
</script>

<div class="flex flex-col h-full">
  <!-- Header -->
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20 shrink-0">
    <button onclick={() => navigate("home")}
            class="text-lg hover:text-mpx-orange transition-colors cursor-pointer">‹</button>
    <h2 class="font-semibold">WiFi Configuration</h2>
    <button onclick={fetchStatus}
            class="ml-auto text-xs text-mpx-muted hover:text-mpx-text transition-colors cursor-pointer">
      ↻ Refresh
    </button>
  </header>

  <div class="flex-1 overflow-y-auto px-4 py-3 space-y-5">
    {#if loading && !reconnecting}
      <p class="text-center text-mpx-muted text-sm mt-8">Loading status…</p>

    {:else if reconnecting}
      <!-- ═══════════════════════════════════════════════════════
           Reconfiguring — AP went down, waiting for it to come back
           ═══════════════════════════════════════════════════════ -->
      <div class="flex flex-col items-center gap-4 py-8 px-4 text-center">
        <div class="w-12 h-12 rounded-full bg-yellow-500/20 border-2 border-yellow-500
                    flex items-center justify-center animate-pulse">
          <span class="text-2xl">📡</span>
        </div>
        <h3 class="text-base font-semibold text-yellow-300">Reconfiguring Network</h3>
        <p class="text-sm text-mpx-muted max-w-xs">
          The robot is restarting its network to connect to <strong class="text-mpx-text">{staSsid}</strong>.
        </p>

        <div class="flex items-center gap-2 text-xs text-mpx-muted">
          <span class="w-2 h-2 rounded-full bg-yellow-500 animate-pulse"></span>
          <span>Waiting for <span class="font-mono">MPX-Dog</span> AP to restart…</span>
        </div>

        <div class="w-full max-w-xs bg-blue-900/30 border border-blue-700/50 rounded-xl p-4 text-left text-xs text-blue-200 space-y-2 mt-2">
          <p class="font-semibold text-blue-100">📌 Your device may have disconnected from the robot.</p>
          <ol class="list-decimal list-inside space-y-1 text-blue-200/80">
            <li>Open your <strong class="text-blue-100">WiFi settings</strong></li>
            <li>Reconnect to <span class="font-mono bg-blue-950 px-1 rounded">MPX-Dog</span></li>
            <li>Return here — the page will update automatically</li>
          </ol>
        </div>

        <p class="text-xs text-mpx-muted">Checking every 2 seconds…</p>

        <button onclick={() => { if (pollTimer) clearTimeout(pollTimer); reconnecting = false; connecting = false; fetchStatus(); }}
                class="text-xs text-mpx-muted hover:text-mpx-text underline cursor-pointer">
          Cancel &amp; try again
        </button>
      </div>

    {:else if error}
      <p class="text-center text-red-400 text-sm mt-8">{error}</p>
      <div class="text-center">
        <button onclick={fetchStatus}
                class="rounded-lg bg-mpx-orange px-6 py-2 text-sm text-white cursor-pointer">
          Retry
        </button>
      </div>

    {:else}

      <!-- ═══════════════════════════════════════════════════════
           AP Mode (always active)
           ═══════════════════════════════════════════════════════ -->
      <section class="rounded-xl bg-mpx-surface border border-mpx-muted/10 p-4">
        <h3 class="text-sm font-semibold uppercase tracking-wide text-mpx-muted mb-3">
          🔵 Access Point (AP) Mode
        </h3>
        <div class="space-y-2 text-sm">
          <div class="flex justify-between">
            <span class="text-mpx-muted">SSID</span>
            <span class="font-mono">{apSsid}</span>
          </div>
          <div class="flex justify-between">
            <span class="text-mpx-muted">IP Address</span>
            <span class="font-mono">{apIp}</span>
          </div>
        </div>
        <p class="mt-3 text-xs text-mpx-muted">
          The robot's built-in network for direct control.
          Devices connect here to access the control panel.
        </p>
      </section>

      <!-- ═══════════════════════════════════════════════════════
           STA Mode (WiFi client)
           ═══════════════════════════════════════════════════════ -->
      <section class="rounded-xl bg-mpx-surface border border-mpx-muted/10 p-4">
        <h3 class="text-sm font-semibold uppercase tracking-wide text-mpx-muted mb-3">
          📡 Station (STA) Mode — Local Network
        </h3>

        <!-- Status -->
        <div class="flex items-center gap-2 mb-4">
          <span class="w-2.5 h-2.5 rounded-full {staBadgeClass()}"></span>
          <span class="text-sm">{staLabel()}</span>
          {#if staIp}
            <span class="text-xs font-mono text-mpx-muted ml-auto">{staIp}</span>
          {/if}
        </div>

        {#if staState === "connected"}
          <!-- Connected — show info and actions -->
          <div class="space-y-2 text-sm mb-4">
            <div class="flex justify-between">
              <span class="text-mpx-muted">Network</span>
              <span class="font-mono">{staSsid}</span>
            </div>
          </div>
          <div class="flex gap-2">
            <button onclick={doDisconnect}
                    class="flex-1 rounded-lg bg-yellow-700 hover:bg-yellow-600 px-4 py-2 text-sm text-white cursor-pointer transition-colors">
              Disconnect
            </button>
            <button onclick={doForget}
                    class="flex-1 rounded-lg bg-red-800 hover:bg-red-700 px-4 py-2 text-sm text-white cursor-pointer transition-colors">
              Forget Network
            </button>
          </div>

        {:else if staState === "connecting"}
          <!-- Connecting — show spinner -->
          <div class="text-center py-4">
            <p class="text-sm text-mpx-muted">Connecting to <span class="font-mono">{staSsid}</span>…</p>
            <p class="text-xs text-mpx-muted mt-1">The robot will appear on your local network once connected.</p>
          </div>

        {:else}
          <!-- Disconnected / Failed — show connect form -->
          {#if connectError}
            <div class="bg-red-900/30 border border-red-700/50 rounded-lg px-3 py-2 mb-3">
              <p class="text-xs text-red-300">{connectError}</p>
            </div>
          {/if}

          <div class="space-y-3">
            <div>
              <label for="ssid" class="block text-xs text-mpx-muted mb-1">Network SSID</label>
              <input id="ssid" type="text" bind:value={inputSsid}
                     placeholder="Enter WiFi network name"
                     class="w-full rounded-lg bg-mpx-bg border border-mpx-muted/20 px-3 py-2 text-sm
                            text-mpx-text placeholder:text-mpx-muted/50
                            focus:outline-none focus:border-mpx-orange/50
                            {connecting ? 'opacity-50' : ''}"
                     disabled={connecting} />
            </div>
            <div>
              <label for="password" class="block text-xs text-mpx-muted mb-1">Password</label>
              <input id="password" type="password" bind:value={inputPassword}
                     placeholder="Leave empty for open network"
                     class="w-full rounded-lg bg-mpx-bg border border-mpx-muted/20 px-3 py-2 text-sm
                            text-mpx-text placeholder:text-mpx-muted/50
                            focus:outline-none focus:border-mpx-orange/50
                            {connecting ? 'opacity-50' : ''}"
                     disabled={connecting} />
            </div>
            <button onclick={doConnect}
                    disabled={connecting || !inputSsid.trim()}
                    class="w-full rounded-lg bg-mpx-orange hover:bg-orange-600
                           disabled:bg-mpx-muted/30 disabled:cursor-not-allowed
                           px-4 py-2.5 text-sm text-white font-medium cursor-pointer transition-colors">
              {connecting ? 'Connecting…' : 'Connect'}
            </button>
          </div>
        {/if}
      </section>

      <!-- ═══════════════════════════════════════════════════════
           Info
           ═══════════════════════════════════════════════════════ -->
      <section class="rounded-xl bg-mpx-surface border border-mpx-muted/10 p-4">
        <h3 class="text-sm font-semibold uppercase tracking-wide text-mpx-muted mb-2">
          ℹ️ Dual-Network Topology
        </h3>
        <ul class="text-xs text-mpx-muted space-y-1.5 list-disc list-inside">
          <li><strong class="text-mpx-text">AP mode</strong> — always active. Direct connection to the robot at <span class="font-mono">{apIp}</span>.</li>
          <li><strong class="text-mpx-text">STA mode</strong> — optional. Connect the robot to your home/office WiFi for internet and LAN access.</li>
          <li>Both networks operate simultaneously. The robot is reachable on both IPs.</li>
        </ul>
      </section>

    {/if}
  </div>
</div>
