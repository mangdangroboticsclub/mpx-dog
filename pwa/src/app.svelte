<script>
  import Home from "./lib/Home.svelte";
  import Chat from "./lib/Chat.svelte";
  import Control from "./lib/Control.svelte";
  import Skills from "./lib/Skills.svelte";
  import SkillsMarketplace from "./lib/SkillsMarketplace.svelte";
  import FileViewer from "./lib/FileViewer.svelte";
  import Upload from "./lib/Upload.svelte";
  import WiFi from "./lib/WiFi.svelte";
  import LuaEditor from "./lib/LuaEditor.svelte";

  let screen = $state("home");

  // ── Network status (polled from robot) ──────────────────────
  let apIp = $state("192.168.2.1");
  let staState = $state("disconnected");
  let staSsid = $state("");
  let staIp = $state("");
  let networkLoaded = $state(false);

  let pollTimer;

  // ── Global permission dialog ────────────────────────────────
  let permissionWs = $state(null);
  let pendingActions = $state([]);

  const PERMISSION_WS_URL = `ws://${location.host}/v1/chat/ui`;

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
        // Reconnect after 3s
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

  async function pollNetworkStatus() {
    try {
      const res = await fetch("/v1/wifi/status");
      if (res.ok) {
        const data = await res.json();
        apIp = data.ap?.ip ?? "192.168.2.1";
        staState = data.sta?.state ?? "disconnected";
        staSsid = data.sta?.ssid ?? "";
        staIp = data.sta?.ip ?? "";
        networkLoaded = true;
      }
    } catch {
      // Robot unreachable — keep last known state
    }
  }

  // ── Derived helpers ─────────────────────────────────────────
  function staColor() {
    switch (staState) {
      case "connected":    return "text-green-400";
      case "connecting":   return "text-yellow-400";
      case "failed":       return "text-red-400";
      default:             return "text-gray-400";
    }
  }

  function staDot() {
    switch (staState) {
      case "connected":    return "bg-green-400";
      case "connecting":   return "bg-yellow-400";
      case "failed":       return "bg-red-500";
      default:             return "bg-gray-500";
    }
  }

  function staLabel() {
    switch (staState) {
      case "connected":    return staIp || "Connected";
      case "connecting":   return "Connecting…";
      case "failed":       return "Failed";
      default:             return "Off";
    }
  }

  function navigate(target) {
    screen = target;
  }

  // ── Init: poll on mount, refresh every 15 s ─────────────────
  $effect(() => {
    pollNetworkStatus();
    pollTimer = setInterval(pollNetworkStatus, 15_000);
    connectPermissionWs();
    return () => {
      clearInterval(pollTimer);
      if (permissionWs) permissionWs.close();
    };
  });
</script>

<div class="h-dvh bg-mpx-bg text-mpx-text flex flex-col overflow-hidden">
  <!-- Top Status Bar (visible on home, subtle on sub-screens) -->
  {#if screen === "home"}
    <header class="flex items-center justify-between px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20">
      <h1 class="text-lg font-bold tracking-wide flex items-center gap-2">
        <span class="text-mpx-orange">◆</span>
        MPX-Dog
      </h1>

      <div class="flex items-center gap-3 text-xs">
        <span class="flex items-center gap-1 text-green-400" title="AP mode — always active">
          <span class="w-2 h-2 rounded-full bg-green-400"></span>
          AP
        </span>
        <span class="flex items-center gap-1 {staColor()}" title="STA mode — local network">
          <span class="w-2 h-2 rounded-full {staDot()} {staState === 'connecting' ? 'animate-pulse' : ''}"></span>
          STA: {staLabel()}
        </span>
      </div>
    </header>

    <!-- Network alert -->
    {#if staState === "connected"}
      <div class="w-full bg-green-900/30 border-b border-green-700/50 px-4 py-2 text-xs text-green-300 text-center">
        ✅ Robot connected to <strong>{staSsid}</strong> · LAN: {staIp} · AP: {apIp}
      </div>
    {:else if staState === "connecting"}
      <div class="w-full bg-yellow-900/30 border-b border-yellow-700/50 px-4 py-2 text-xs text-yellow-300 text-center">
        ⏳ Connecting to <strong>{staSsid}</strong>…
      </div>
    {:else}
      <div class="w-full bg-yellow-900/30 border-b border-yellow-700/50 px-4 py-2 text-xs text-yellow-300 text-center">
        ⚠ AP mode active at {apIp} · Configure WiFi in Settings to enable LAN/internet access
      </div>
    {/if}
  {/if}

  <!-- Screen content -->
  <main class="flex-1 flex flex-col overflow-hidden">
    {#if screen === "home"}
      <Home {navigate} {apIp} {staState} {staSsid} {staIp} />
    {:else if screen === "chat"}
      <Chat {navigate} />
    {:else if screen === "control"}
      <Control {navigate} />
    {:else if screen === "skills"}
      <Skills {navigate} />
    {:else if screen === "wasm"}
      <Skills {navigate} />
    {:else if screen === "marketplace"}
      <SkillsMarketplace {navigate} />
    {:else if screen === "files"}
      <FileViewer {navigate} />
    {:else if screen === "upload"}
      <Upload {navigate} />
    {:else if screen === "wifi"}
      <WiFi {navigate} />
    {:else if screen === "lua"}
      <LuaEditor {navigate} />
    {/if}
  </main>

  <!-- Global permission dialog overlay -->
  {#if pendingActions.length > 0}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-4"
         onclick={() => {}}>
      <div class="w-full max-w-sm rounded-2xl bg-mpx-surface border border-mpx-muted/20 p-5 shadow-2xl"
           onclick={(e) => e.stopPropagation()}>
        <h3 class="text-base font-bold text-mpx-text mb-1">Permission Required</h3>
        <p class="text-xs text-mpx-muted mb-4">
          The robot is requesting access to perform an action.
        </p>

        {#each pendingActions as action}
          <div class="rounded-lg bg-mpx-bg/50 border border-mpx-muted/10 px-4 py-3 mb-4">
            <div class="flex items-center gap-2 mb-1">
              <span class="text-lg">🔧</span>
              <span class="text-sm font-semibold text-mpx-text capitalize">{action.type.replace(/_/g, " ")}</span>
            </div>
            <p class="text-xs text-mpx-muted ml-8">{action.description}</p>
          </div>
        {/each}

        <div class="flex gap-3">
          <button
            onclick={() => respondPermission(pendingActions[0].id, false)}
            class="flex-1 rounded-lg border border-red-500/50 text-red-400 px-4 py-2.5 text-sm font-medium
                   hover:bg-red-500/10 transition-colors cursor-pointer"
          >⛔ Deny</button>
          <button
            onclick={() => respondPermission(pendingActions[0].id, true)}
            class="flex-1 rounded-lg bg-emerald-600 px-4 py-2.5 text-sm font-medium text-white
                   hover:bg-emerald-500 transition-colors cursor-pointer"
          >✅ Approve</button>
        </div>
      </div>
    </div>
  {/if}
</div>
