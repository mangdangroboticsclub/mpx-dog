<script>
  import { colors } from "./colors.js";

  /** @type {{ onNavigate?: (view: string) => void, network?: object }} */
  let { onNavigate, network } = $props();

  const YELLOW = colors.mpx.primary;

  // ── State ──────────────────────────────────────────────────
  let loading = $state(true);
  let error = $state("");

  // AP info
  let apSsid = $state(network?.apSsid || "MPX-Dog");
  let apIp = $state(network?.apIp || "192.168.2.1");

  // STA info
  let staState = $state(network?.staState || "disconnected");
  let staSsid = $state(network?.staSsid || "");
  let staIp = $state(network?.staIp || "");

  // Connect form
  let inputSsid = $state("");
  let inputPassword = $state("");

  // 802.1X (WPA2/WPA3-Enterprise). School and campus networks hand out an
  // account rather than a shared passphrase, so SSID + password is not
  // enough on its own — the robot also needs a username, and sometimes a
  // separate outer identity that travels in the clear before the tunnel.
  let enterprise = $state(false);
  let eapMethod = $state("peap");
  let eapUsername = $state("");
  let eapIdentity = $state("");
  let eapPhase2 = $state("mschapv2");
  let showIdentity = $state(false);
  let connecting = $state(false);
  let reconnecting = $state(false);
  let connectError = $state("");

  // Poll timer for reconnection
  let pollTimer = $state(null);

  // ── Derived ─────────────────────────────────────────────────
  let wifiOn = $derived(staState !== "disconnected");
  let showConnectForm = $state(false);

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
        // Re-open the form on the setting the robot is actually using. The
        // firmware returns the identity (it goes over the air in the clear
        // anyway) but never the password, so that field always starts empty.
        if (data.sta?.enterprise) {
          enterprise = true;
          eapMethod = data.sta?.eap_method ?? "peap";
          if (!eapUsername && data.sta?.identity) eapUsername = data.sta.identity;
        }
        if (reconnecting) reconnecting = false;
      } else if (!reconnecting) {
        error = `Status fetch failed (${res.status})`;
      }
    } catch (e) {
      if (!reconnecting) error = `Cannot reach robot: ${e.message}`;
    }
    loading = false;
  }

  function startReconnectPoller() {
    if (pollTimer) clearTimeout(pollTimer);
    async function poll() {
      try {
        const res = await fetch("/v1/wifi/status");
        if (res.ok) {
          const data = await res.json();
          const state = data.sta?.state ?? "disconnected";
          staState = state;
          staSsid = data.sta?.ssid ?? staSsid;
          staIp = data.sta?.ip ?? "";
          if (state === "connected" || state === "failed" || state === "disconnected") {
            reconnecting = false;
            connecting = false;
            return;
          }
        }
      } catch {}
      pollTimer = setTimeout(poll, 2000);
    }
    poll();
  }

  function buildConnectBody() {
    const body = { ssid: inputSsid.trim(), password: inputPassword };
    if (!enterprise) return body;

    body.eap_method = eapMethod;
    body.username = eapUsername.trim();
    // Blank outer identity means "use the username", which is what the
    // firmware does and what a phone does when you leave the anonymous
    // identity empty. Only send it when the user actually filled it in.
    if (eapIdentity.trim()) body.identity = eapIdentity.trim();
    if (eapMethod === "ttls") body.phase2 = eapPhase2;
    return body;
  }

  async function doConnect() {
    const ssid = inputSsid.trim();
    if (!ssid) return;
    if (enterprise && !eapUsername.trim()) {
      connectError = "Enterprise networks need a username.";
      return;
    }
    connecting = true;
    reconnecting = false;
    connectError = "";
    let requestSent = false;
    try {
      const res = await fetch("/v1/wifi/connect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(buildConnectBody()),
      });
      requestSent = true;
      if (res.ok) {
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
      if (!requestSent) {
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
      showConnectForm = false;
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
      showConnectForm = false;
    } catch (e) {
      console.error("Forget failed:", e);
    }
  }

  function toggleWifi() {
    if (wifiOn) {
      doDisconnect();
    } else {
      showConnectForm = true;
    }
  }

  function goBack() {
    onNavigate?.("welcome");
  }

  // ── Init ───────────────────────────────────────────────────
  $effect(() => {
    fetchStatus();
    return () => { if (pollTimer) clearTimeout(pollTimer); };
  });
</script>

<div class="wifi-root">
  <!-- ═══ Header ═══ -->
  <div class="header">
    <button class="back-btn" onclick={goBack} aria-label="Back">
      <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <path d="M19 12H5M12 19l-7-7 7-7"/>
      </svg>
    </button>
    <h1 class="header-title">WiFi</h1>
    <button onclick={fetchStatus} class="refresh-btn" aria-label="Refresh">↻</button>
  </div>

  <!-- ═══ Loading ═══ -->
  {#if loading && !reconnecting}
    <div class="scanning">
      <span class="spinner"></span>
      <span>Loading status…</span>
    </div>
  {:else if reconnecting}
    <!-- ═══ Reconfiguring ═══ -->
    <div class="reconnecting">
      <div class="recon-icon">📡</div>
      <h3 class="recon-title">Reconfiguring Network</h3>
      <p class="recon-desc">
        The robot is restarting its network to connect to <strong>{staSsid}</strong>.
      </p>
      <p class="recon-hint">
        Your device may have disconnected. Reconnect to <strong>MPX-Dog</strong> WiFi if needed.
      </p>
      <button class="cancel-btn" onclick={() => { if (pollTimer) clearTimeout(pollTimer); reconnecting = false; connecting = false; fetchStatus(); }}>
        Cancel
      </button>
    </div>
  {:else if error}
    <div class="scanning" style="color: #d00">{error}</div>
    <button class="retry-btn" onclick={fetchStatus}>Retry</button>
  {:else}
    <!-- ═══ AP Info ═══ -->
    <div class="info-card">
      <div class="info-row">
        <span class="info-label">🔵 AP Mode</span>
        <span class="info-value">{apSsid} · {apIp}</span>
      </div>
    </div>

    <!-- ═══ STA Status ═══ -->
    <div class="info-card">
      <div class="info-row">
        <span class="info-label">📡 Station Mode</span>
        <span class="info-value">
          {#if staState === "connected"}
            <span class="status-dot status-ok"></span> {staSsid} · {staIp}
          {:else if staState === "connecting"}
            <span class="status-dot status-busy"></span> Connecting to {staSsid}…
          {:else if staState === "failed"}
            <span class="status-dot status-err"></span> Failed
          {:else}
            <span class="status-dot status-off"></span> Disconnected
          {/if}
        </span>
      </div>
    </div>

    <!-- ═══ Toggle & Actions ═══ -->
    <div class="wifi-toggle-card">
      <div class="toggle-row">
        <div class="toggle-label">
          <svg class="toggle-icon" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M5 12.55a11 11 0 0 1 14.08 0"/>
            <path d="M1.42 9a16 16 0 0 1 21.16 0"/>
            <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/>
            <circle cx="12" cy="20" r="1" fill="currentColor"/>
          </svg>
          <span>Wi‑Fi</span>
        </div>
        <button
          class="toggle-switch"
          class:active={wifiOn}
          onclick={toggleWifi}
          role="switch"
          aria-checked={wifiOn}
          aria-label={wifiOn ? "Disable Wi-Fi" : "Enable Wi-Fi"}
        >
          <span class="toggle-knob"></span>
        </button>
      </div>
    </div>

    {#if wifiOn}
      <button class="action-btn" onclick={doForget}>Forget Network</button>
    {/if}

    <!-- ═══ Connect Form ═══ -->
    {#if showConnectForm || !wifiOn}
      <div class="connect-form">
        <p class="form-hint">Connect to a Wi‑Fi network to give your robot internet access.</p>

        <div class="field">
          <label class="field-label" for="wifi-ssid">Network Name (SSID)</label>
          <input
            id="wifi-ssid"
            class="field-input"
            type="text"
            bind:value={inputSsid}
            placeholder="Enter network name"
          />
        </div>

        <!-- ── 802.1X toggle ── -->
        <div class="ent-row">
          <div class="ent-label">
            <span>Enterprise (802.1X)</span>
            <span class="ent-sub">School or work network with a login</span>
          </div>
          <button
            class="toggle-switch small"
            class:active={enterprise}
            onclick={() => (enterprise = !enterprise)}
            role="switch"
            aria-checked={enterprise}
            aria-label="Enterprise network"
          >
            <span class="toggle-knob"></span>
          </button>
        </div>

        {#if enterprise}
          <div class="field">
            <label class="field-label" for="wifi-eap">EAP method</label>
            <select id="wifi-eap" class="field-input" bind:value={eapMethod}>
              <option value="peap">PEAP · MSCHAPv2 (most schools)</option>
              <option value="ttls">TTLS</option>
            </select>
          </div>

          {#if eapMethod === "ttls"}
            <div class="field">
              <label class="field-label" for="wifi-phase2">Phase 2</label>
              <select id="wifi-phase2" class="field-input" bind:value={eapPhase2}>
                <option value="mschapv2">MSCHAPv2</option>
                <option value="pap">PAP</option>
                <option value="mschap">MSCHAP</option>
                <option value="chap">CHAP</option>
              </select>
            </div>
          {/if}

          <div class="field">
            <label class="field-label" for="wifi-user">Username</label>
            <input
              id="wifi-user"
              class="field-input"
              type="text"
              autocapitalize="none"
              autocorrect="off"
              spellcheck="false"
              bind:value={eapUsername}
              placeholder="e.g. s1234567@school.edu"
            />
          </div>

          <button class="link-btn" onclick={() => (showIdentity = !showIdentity)}>
            {showIdentity ? "Hide" : "Add"} anonymous identity (optional)
          </button>

          {#if showIdentity}
            <div class="field">
              <label class="field-label" for="wifi-ident">Anonymous identity</label>
              <input
                id="wifi-ident"
                class="field-input"
                type="text"
                autocapitalize="none"
                autocorrect="off"
                spellcheck="false"
                bind:value={eapIdentity}
                placeholder="anonymous@school.edu"
              />
              <p class="field-hint">
                Sent before the secure tunnel opens. Leave blank to use your username.
              </p>
            </div>
          {/if}
        {/if}

        <div class="field">
          <label class="field-label" for="wifi-password">Password</label>
          <input
            id="wifi-password"
            class="field-input"
            type="password"
            bind:value={inputPassword}
            placeholder="Enter password"
          />
        </div>

        {#if enterprise}
          <p class="field-hint">
            The robot will not check the school's server certificate. If the
            login fails, the log usually says reason=23, which means the
            school rejected the username or password.
          </p>
        {/if}

        <button
          class="join-btn"
          onclick={doConnect}
          disabled={connecting || !inputSsid.trim() || (enterprise && !eapUsername.trim())}
        >
          {#if connecting}
            <span class="spinner"></span>
          {:else}
            Connect
          {/if}
        </button>
      </div>
    {/if}

    <!-- ═══ Error Toast ═══ -->
    {#if connectError}
      <div class="error-toast">
        <span>{connectError}</span>
        <button class="error-ok" onclick={() => connectError = ""}>Ok</button>
      </div>
    {/if}
  {/if}
</div>

<style>
  .wifi-root {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
    padding: 0 20px;
    overflow-y: auto;
  }

  /* ── Header ─────────────────────────────── */
  .header {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 16px 0 8px;
  }

  .back-btn {
    width: 36px;
    height: 36px;
    border: none;
    background: transparent;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: #000;
    flex-shrink: 0;
  }
  .back-btn:hover {
    background: rgba(0,0,0,0.05);
  }

  .header-title {
    font-size: 1.25rem;
    font-weight: 700;
    color: #000;
  }

  /* ── Toggle Card ────────────────────────── */
  .wifi-toggle-card {
    background: #f5f5f5;
    border-radius: 14px;
    padding: 12px 16px;
    margin: 8px 0 12px;
  }

  .toggle-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .toggle-label {
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 1rem;
    font-weight: 600;
    color: #000;
  }

  .toggle-icon {
    color: #000;
  }

  .toggle-switch {
    width: 48px;
    height: 28px;
    border-radius: 14px;
    border: none;
    background: #ccc;
    cursor: pointer;
    position: relative;
    transition: background 0.2s ease;
    padding: 0;
  }
  .toggle-switch.active {
    background: #000;
  }

  .toggle-knob {
    position: absolute;
    top: 3px;
    left: 3px;
    width: 22px;
    height: 22px;
    border-radius: 50%;
    background: #fff;
    box-shadow: 0 1px 3px rgba(0,0,0,0.2);
    transition: transform 0.2s ease;
  }
  .toggle-switch.active .toggle-knob {
    transform: translateX(20px);
  }

  /* ── Scanning ───────────────────────────── */
  .scanning {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 20px 0;
    font-size: 0.9rem;
    color: #666;
  }

  .spinner {
    width: 18px;
    height: 18px;
    border: 2.5px solid #ccc;
    border-top-color: #000;
    border-radius: 50%;
    animation: spin 0.7s linear infinite;
    display: inline-block;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  /* ── Section Label ──────────────────────── */
  .section-label {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    margin: 12px 0 8px;
  }

  /* ── Network List ───────────────────────── */
  .network-list {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .network-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 16px;
    background: #f5f5f5;
    border: none;
    border-radius: 12px;
    cursor: pointer;
    text-align: left;
    transition: background 0.15s;
  }
  .network-item:hover {
    background: #eee;
  }
  .network-item:active {
    background: #e5e5e5;
  }

  .net-info {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .net-icon {
    color: #000;
    flex-shrink: 0;
  }

  .net-ssid {
    font-size: 1rem;
    font-weight: 500;
    color: #000;
  }

  .net-meta {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .lock-icon {
    color: #999;
  }

  .signal-bars {
    display: flex;
    align-items: flex-end;
    gap: 2px;
    height: 14px;
  }

  .signal-bar {
    width: 4px;
    border-radius: 2px;
    background: #ddd;
  }
  .signal-bar:nth-child(1) { height: 5px; }
  .signal-bar:nth-child(2) { height: 8px; }
  .signal-bar:nth-child(3) { height: 11px; }
  .signal-bar:nth-child(4) { height: 14px; }
  .signal-bar.active {
    background: #000;
  }

  /* ── Connect Form ───────────────────────── */
  .connect-form {
    margin-top: 8px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }

  .form-hint {
    font-size: 0.85rem;
    color: #666;
    line-height: 1.5;
  }

  .field {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .field-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
  }

  .field-input {
    width: 100%;
    padding: 12px 14px;
    border: 1.5px solid #ddd;
    border-radius: 10px;
    font-size: 1rem;
    color: #000;
    background: #fff;
    outline: none;
    transition: border-color 0.15s;
  }
  .field-input:focus {
    border-color: #000;
  }
  .field-input::placeholder {
    color: #bbb;
  }

  .join-btn {
    align-self: flex-end;
    width: 48px;
    height: 48px;
    border-radius: 50%;
    border: none;
    background: #000;
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s, transform 0.1s;
    margin-top: 8px;
  }
  .join-btn:hover {
    background: #222;
  }
  .join-btn:active {
    transform: scale(0.95);
  }
  .join-btn:disabled {
    background: #ccc;
    cursor: not-allowed;
  }

  /* ── Enterprise toggle ──────────────────── */
  .ent-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    background: #f5f5f5;
    border-radius: 12px;
    padding: 12px 14px;
  }

  .ent-label {
    display: flex;
    flex-direction: column;
    gap: 2px;
    font-size: 0.9rem;
    font-weight: 600;
    color: #000;
  }

  .ent-sub {
    font-size: 0.75rem;
    font-weight: 400;
    color: #888;
  }

  .toggle-switch.small {
    width: 42px;
    height: 24px;
    border-radius: 12px;
    flex-shrink: 0;
  }
  .toggle-switch.small .toggle-knob {
    width: 18px;
    height: 18px;
  }
  .toggle-switch.small.active .toggle-knob {
    transform: translateX(18px);
  }

  select.field-input {
    appearance: none;
    background-image: url("data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%23666' stroke-width='2' fill='none' stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: right 14px center;
    padding-right: 36px;
    cursor: pointer;
  }

  .link-btn {
    align-self: flex-start;
    background: none;
    border: none;
    padding: 0;
    font-size: 0.8rem;
    font-weight: 600;
    color: #666;
    text-decoration: underline;
    cursor: pointer;
  }
  .link-btn:hover { color: #000; }

  .field-hint {
    font-size: 0.75rem;
    color: #888;
    line-height: 1.45;
  }

  /* ── Error Toast ────────────────────────── */
  .error-toast {
    position: fixed;
    bottom: 24px;
    left: 20px;
    right: 20px;
    background: #fff;
    border-radius: 14px;
    padding: 16px 20px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    box-shadow: 0 4px 24px rgba(0,0,0,0.15);
    font-size: 0.9rem;
    color: #000;
    z-index: 100;
    animation: slideUp 0.3s ease;
  }

  @keyframes slideUp {
    from { transform: translateY(20px); opacity: 0; }
    to { transform: translateY(0); opacity: 1; }
  }

  .error-ok {
    padding: 6px 16px;
    border: none;
    border-radius: 8px;
    background: #000;
    color: #fff;
    font-weight: 600;
    font-size: 0.85rem;
    cursor: pointer;
  }

  /* ── Status dots ────────────────────────── */
  .status-dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-right: 4px;
  }
  .status-ok { background: #22c55e; }
  .status-busy { background: #eab308; animation: pulse 1s infinite; }
  .status-err { background: #ef4444; }
  .status-off { background: #999; }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }

  /* ── Info cards ─────────────────────────── */
  .info-card {
    background: #f5f5f5;
    border-radius: 14px;
    padding: 12px 16px;
    margin: 4px 0;
  }

  .info-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .info-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
    flex-shrink: 0;
  }

  .info-value {
    font-size: 0.8rem;
    color: #666;
    text-align: right;
    word-break: break-all;
  }

  /* ── Refresh / Retry / Action buttons ──── */
  .refresh-btn {
    background: none;
    border: none;
    font-size: 1.2rem;
    cursor: pointer;
    color: #666;
    padding: 4px 8px;
    border-radius: 6px;
  }
  .refresh-btn:hover { background: #eee; }

  .retry-btn {
    display: block;
    margin: 8px auto;
    padding: 8px 24px;
    border: none;
    border-radius: 10px;
    background: #000;
    color: #fff;
    font-weight: 600;
    cursor: pointer;
  }

  .action-btn {
    display: block;
    width: 100%;
    padding: 10px;
    margin-top: 6px;
    border: none;
    border-radius: 10px;
    background: #f5f5f5;
    color: #d00;
    font-weight: 600;
    font-size: 0.9rem;
    cursor: pointer;
  }
  .action-btn:hover { background: #fee; }

  .cancel-btn {
    display: block;
    margin: 8px auto;
    padding: 6px 20px;
    border: 1px solid #ccc;
    border-radius: 8px;
    background: #fff;
    color: #666;
    font-size: 0.85rem;
    cursor: pointer;
  }

  /* ── Reconnecting state ─────────────────── */
  .reconnecting {
    text-align: center;
    padding: 30px 16px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
  }

  .recon-icon {
    font-size: 2.5rem;
    animation: pulse 1.5s infinite;
  }

  .recon-title {
    font-size: 1.1rem;
    font-weight: 700;
    color: #000;
  }

  .recon-desc {
    font-size: 0.85rem;
    color: #666;
    line-height: 1.5;
  }

  .recon-hint {
    font-size: 0.8rem;
    color: #999;
    background: #f9f9f9;
    padding: 10px 14px;
    border-radius: 10px;
    line-height: 1.5;
  }

  /* ── Join button (wider) ────────────────── */
  .join-btn {
    align-self: stretch;
    width: auto;
    height: auto;
    padding: 12px;
    border-radius: 12px;
    border: none;
    background: #000;
    color: #fff;
    font-weight: 700;
    font-size: 1rem;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    cursor: pointer;
    transition: background 0.15s, transform 0.1s;
    margin-top: 8px;
  }
  .join-btn:hover {
    background: #222;
  }
  .join-btn:active {
    transform: scale(0.98);
  }
  .join-btn:disabled {
    background: #ccc;
    cursor: not-allowed;
  }
</style>
