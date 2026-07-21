<script>
  import { colors } from "./lib/colors.js";
  import LockScreen from "./lib/LockScreen.svelte";

  const YELLOW = colors.mpx.primary;

  // ── Network status (polled from robot) ──────────────────────
  let apIp = $state("192.168.2.1");
  let apSsid = $state("MPX-Dog");
  let staState = $state("disconnected");
  let staSsid = $state("");
  let staIp = $state("");
  let networkLoaded = $state(false);

  let pollTimer;

  async function pollNetworkStatus() {
    try {
      const res = await fetch("/v1/wifi/status");
      if (res.ok) {
        const data = await res.json();
        apIp = data.ap?.ip ?? "192.168.2.1";
        apSsid = data.ap?.ssid ?? "MPX-Dog";
        staState = data.sta?.state ?? "disconnected";
        staSsid = data.sta?.ssid ?? "";
        staIp = data.sta?.ip ?? "";
        networkLoaded = true;
      }
    } catch {
      // Robot unreachable — keep last known state
    }
  }

  // ── Global permission WebSocket ─────────────────────────────
  // Listens for openclaw_action events (file_write, wasm_run, etc.)
  // and shows a modal so the user can approve/deny from any screen.
  const PERMISSION_WS_URL = `ws://${location.host}/v1/chat/ui`;

  let permissionWs = $state(null);
  let pendingActions = $state([]);

  function connectPermissionWs() {
    if (permissionWs && permissionWs.readyState === WebSocket.OPEN) return;

    try {
      const ws = new WebSocket(PERMISSION_WS_URL);
      ws.onopen = () => { permissionWs = ws; };
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (data.type === "openclaw_action" && data.action_id) {
            if (data.status === "pending") {
              pendingActions = [...pendingActions, {
                id: data.action_id,
                type: data.action_type || "unknown",
                description: data.description || "",
              }];
            } else {
              pendingActions = pendingActions.filter((a) => a.id !== data.action_id);
            }
          }
        } catch { /* ignore malformed JSON */ }
      };
      ws.onclose = () => {
        permissionWs = null;
        setTimeout(connectPermissionWs, 3000);
      };
      ws.onerror = () => { ws.close(); };
    } catch { /* ignore */ }
  }

  function respondPermission(actionId, approved) {
    if (permissionWs && permissionWs.readyState === WebSocket.OPEN) {
      permissionWs.send(JSON.stringify({
        type: "permission_response",
        action_id: actionId,
        approved,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
    pendingActions = pendingActions.filter((a) => a.id !== actionId);
  }

  // ── Poll on mount, refresh every 15 s ───────────────────────
  $effect(() => {
    pollNetworkStatus();
    pollTimer = setInterval(pollNetworkStatus, 15_000);
    connectPermissionWs();
    return () => {
      clearInterval(pollTimer);
      if (permissionWs) permissionWs.close();
    };
  });

  // ── Pack props for children ─────────────────────────────────
  let network = $derived({ apIp, apSsid, staState, staSsid, staIp, networkLoaded });
</script>

<LockScreen {network} />

<!-- ═══ Global Permission Modal Overlay ═══ -->
{#if pendingActions.length > 0}
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div
    class="perm-overlay"
    onclick={() => {}}
  >
    <div
      class="perm-modal"
      style="--yellow: {YELLOW}"
      onclick={(e) => e.stopPropagation()}
    >
      <h3 class="perm-title">Permission Required</h3>
      <p class="perm-subtitle">
        The robot is requesting access to perform an action.
      </p>

      {#each pendingActions as action}
        <div class="perm-action-card">
          <div class="perm-action-header">
            <span class="perm-action-icon">🔧</span>
            <span class="perm-action-type">{action.type.replace(/_/g, " ")}</span>
          </div>
          <p class="perm-action-desc">{action.description}</p>
        </div>
      {/each}

      <div class="perm-buttons">
        <button
          class="perm-btn perm-btn-deny"
          onclick={() => respondPermission(pendingActions[0].id, false)}
        >⛔ Deny</button>
        <button
          class="perm-btn perm-btn-approve"
          onclick={() => respondPermission(pendingActions[0].id, true)}
        >✅ Approve</button>
      </div>
    </div>
  </div>
{/if}

<style>
  /* ── Permission Modal Overlay ──────────── */
  .perm-overlay {
    position: fixed;
    inset: 0;
    z-index: 100;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 20px;
  }

  .perm-modal {
    width: 100%;
    max-width: 320px;
    background: #fff;
    border-radius: 20px;
    padding: 24px 20px 20px;
    box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
  }

  .perm-title {
    font-size: 1.05rem;
    font-weight: 700;
    color: #000;
    margin-bottom: 2px;
  }

  .perm-subtitle {
    font-size: 0.78rem;
    color: #969494;
    margin-bottom: 16px;
  }

  .perm-action-card {
    background: #f5f5f5;
    border-radius: 12px;
    padding: 14px;
    margin-bottom: 14px;
  }

  .perm-action-header {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 4px;
  }

  .perm-action-icon {
    font-size: 1.2rem;
  }

  .perm-action-type {
    font-size: 0.85rem;
    font-weight: 600;
    color: #000;
    text-transform: capitalize;
  }

  .perm-action-desc {
    font-size: 0.75rem;
    color: #888;
    margin-left: 28px;
    line-height: 1.4;
  }

  .perm-buttons {
    display: flex;
    gap: 10px;
  }

  .perm-btn {
    flex: 1;
    padding: 12px 0;
    border-radius: 12px;
    border: none;
    font-size: 0.85rem;
    font-weight: 600;
    cursor: pointer;
    text-align: center;
    transition: filter 0.15s;
  }

  .perm-btn-deny {
    background: #f5f5f5;
    color: #ED7676;
    border: 1.5px solid #ED7676;
  }
  .perm-btn-deny:active { filter: brightness(0.95); }

  .perm-btn-approve {
    background: var(--yellow);
    color: #000;
  }
  .perm-btn-approve:active { filter: brightness(0.9); }
</style>
