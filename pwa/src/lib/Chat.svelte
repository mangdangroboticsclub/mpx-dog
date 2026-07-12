<script>
  import { onMount } from "svelte";
  import { marked } from "marked";
  import ConversationSidebar from "./ConversationSidebar.svelte";
  import {
    saveConversation,
    getConversation,
  } from "./conversationStore.js";

  let { navigate } = $props();

  // ── UUID helper (works in both secure and insecure contexts) ─
  function generateUUID() {
    if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
      return crypto.randomUUID();
    }
    // Fallback for insecure (HTTP) contexts
    return "10000000-1000-4000-8000-100000000000".replace(/[018]/g, (c) =>
      (c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (c / 4)))).toString(16)
    );
  }

  // ── Session ID persistence ───────────────────────────────────
  const SESSION_STORAGE_KEY = "mpx_active_session_id";

  /**
   * Restore the last active session ID from localStorage, or generate
   * a fresh one on first visit.  This ensures the session ID survives
   * component re-mounts (SPA navigation, page refresh) so the server
   * always receives the same identifier — preserving AI context.
   */
  function restoreSessionId() {
    try {
      const saved = localStorage.getItem(SESSION_STORAGE_KEY);
      if (saved) return saved;
    } catch { /* localStorage unavailable — ignore */ }
    return generateUUID();
  }

  // ── State ────────────────────────────────────────────────────
  /** Plain variable (not $state) — avoids reactive cascade from $effect tracking */
  let ws = null;
  let connected = $state(false);
  let sending = $state(false);
  let sessionId = $state(restoreSessionId());
  let messages = $state([]);
  let inputText = $state("");
  let sidebarRefreshKey = $state(0);
  let sessionAckBadge = $state(false);     // show "Session reset" badge in header

  // ── Live timer for response counter ─────────────────────────
  let now = $state(Date.now());
  let lastSentTs = $state(null);
  $effect(() => {
    const id = setInterval(() => { now = Date.now(); }, 200);
    return () => clearInterval(id);
  });

  // ── Permission state ─────────────────────────────────────────
  let pendingActions = $state([]); // { id, type, description, status }

  // Refs for auto-focus and auto-scroll
  let inputEl = $state(null);
  let scrollAnchor = $state(null);

  // Each message is { role, text, status?, history?, commands?, steps?, ts? }
  //   role: "user" | "bot" | "step" | "system"
  //   history: array of { stage, ts, text?, seq?, total? } — timeline entries

  /** Accumulator for chunked chat_reply reassembly — keyed by session_id */
  let pendingChunks = {};

  // ── Markdown renderer ────────────────────────────────────────

  // Configure marked to open links in a new tab
  const renderer = new marked.Renderer();
  renderer.link = ({ href, text }) =>
    `<a href="${href}" target="_blank" rel="noopener noreferrer" class="text-mpx-orange underline hover:brightness-110">${text}</a>`;

  marked.setOptions({ renderer, breaks: true, gfm: true });

  /**
   * Render Markdown text to safe HTML.
   * Script tags and event handler attributes are stripped for safety.
   */
  function renderMarkdown(text) {
    if (!text) return "";
    const raw = marked.parse(text, { async: false });
    // Strip <script> tags and event handlers (onclick=, onerror=, etc.)
    return raw
      .replace(/<script[\s\S]*?<\/script>/gi, "")
      .replace(/\son\w+\s*=\s*["']?[^"'\s>]+["']?/gi, "");
  }

  // ── Timestamp & date helpers ────────────────────────────────

  /** Normalize a timestamp to milliseconds — accepts ms or seconds epoch */
  function normalizeTs(ts) {
    return ts < 100000000000 ? ts * 1000 : ts;
  }

  /** Format a timestamp for display (HH:MM) */
  function formatTimestamp(ts) {
    if (!ts) return "";
    const d = new Date(normalizeTs(ts));
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  /** Extract the best timestamp from a message object */
  function getMsgTs(msg) {
    if (msg.ts) return msg.ts;
    if (msg.history && msg.history.length > 0) return msg.history[0].ts;
    return null;
  }

  /** Check if two messages are on different calendar days */
  function isNewDay(a, b) {
    const tsA = getMsgTs(a);
    const tsB = getMsgTs(b);
    if (!tsA || !tsB) return false;
    const dA = new Date(normalizeTs(tsA));
    const dB = new Date(normalizeTs(tsB));
    return dA.getDate() !== dB.getDate() || dA.getMonth() !== dB.getMonth() || dA.getFullYear() !== dB.getFullYear();
  }

  /**
   * Walk backwards from `fromIdx` to find the nearest message with a valid
   * timestamp. Falls back to `messages[fromIdx]` if none found.
   */
  function findPrevTimestampMsg(messages, fromIdx) {
    for (let i = fromIdx; i >= 0; i--) {
      if (getMsgTs(messages[i])) return messages[i];
    }
    return messages[fromIdx];
  }

  /** Check if a message is from a previous day (not today) */
  function isFromPastDay(msg) {
    const ts = getMsgTs(msg);
    if (!ts) return false;
    const d = new Date(normalizeTs(ts));
    const now = new Date();
    const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const msgDay = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    return msgDay.getTime() !== today.getTime();
  }

  /** Format a date label for the date delineator */
  function formatDateLabel(msg) {
    const ts = getMsgTs(msg);
    if (!ts) return "";
    const d = new Date(normalizeTs(ts));
    const now = new Date();
    const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const dateDay = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    const diffDays = Math.round((today - dateDay) / 86400000);
    if (diffDays === 0) return "Today";
    if (diffDays === 1) return "Yesterday";
    if (diffDays < 7) {
      const days = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
      return days[d.getDay()];
    }
    return d.toLocaleDateString([], { month: "short", day: "numeric", year: "numeric" });
  }

  /** Compute response seconds between a user msg and following bot reply */
  function getResponseSeconds(messages, botIdx) {
    if (botIdx <= 0) return null;
    const botMsg = messages[botIdx];
    if (!botMsg || botMsg.role !== "bot") return null;
    // Search backwards for the preceding user message (skipping step/system in-between)
    let userMsg = null;
    for (let i = botIdx - 1; i >= 0; i--) {
      if (messages[i].role === "user") {
        userMsg = messages[i];
        break;
      }
    }
    if (!userMsg) return null;
    const start = userMsg.ts || userMsg.history?.[0]?.ts;
    const end = botMsg.ts;
    if (!start || !end) return null;
    const diff = normalizeTs(end) - normalizeTs(start);
    if (diff <= 0) return null;
    return (diff / 1000).toFixed(1);
  }

  const WS_URL = `ws://${location.host}/v1/chat/ui`;

  // ── Auto-save ────────────────────────────────────────────────

  // Debounce helper: returns a function that calls `fn` after `ms` of inactivity
  function debounce(fn, ms) {
    let timer;
    return (...args) => {
      clearTimeout(timer);
      timer = setTimeout(() => fn(...args), ms);
    };
  }

  const persist = debounce((id, msgs) => {
    saveConversation(id, { messages: msgs });
    sidebarRefreshKey++;
  }, 500);

  // Watch messages and sessionId — persist whenever they change
  $effect(() => {
    const msgs = messages;
    const id = sessionId;
    if (msgs.length > 0) {
      persist(id, msgs);
    }
  });

  // ── Restore a saved conversation ─────────────────────────────

  function selectConversation(id) {
    const convo = getConversation(id);
    if (!convo) return;

    // Save current conversation first (if it has messages)
    if (messages.length > 0) {
      saveConversation(sessionId, { messages });
      sidebarRefreshKey++;
    }

    // Restore state
    sessionId = convo.id;
    messages = convo.messages || [];
    sending = false;

    // Tell the server about the new session over the EXISTING WebSocket.
    // Do NOT disconnect/reconnect — that races with the server's CLOSE
    // frame handling and exhausts connection slots on the ESP32.
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
  }

  // ── WebSocket lifecycle ──────────────────────────────────────

  /**
   * Create a new WebSocket connection.  Safe to call multiple times —
   * if already open/connecting it returns early.  Sends session_reset
   * as the very first frame after opening so the server always knows
   * the current session ID (fixes race condition with setTimeout).
   */
  function connect() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      return;
    }

    const socket = new WebSocket(WS_URL);

    socket.onopen = () => {
      connected = true;
      // Announce the current session immediately — no fragile timer
      socket.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
      }));
    };

    socket.onclose = () => {
      connected = false;
      ws = null;
      // Auto-reconnect after 3s
      setTimeout(() => {
        if (document.visibilityState !== "hidden") connect();
      }, 3000);
    };

    socket.onerror = () => {
      connected = false;
    };

    socket.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        handleMessage(data);
      } catch {
        // Ignore unparseable frames
      }
    };

    ws = socket;
  }

  function disconnect() {
    if (ws) {
      ws.onclose = null;  // prevent reconnect
      ws.close();
      ws = null;
    }
    connected = false;
  }

  // ── Message handling ─────────────────────────────────────────

  function handleMessage(data) {
    // ── Chunked chat_reply reassembly ──────────────────────────
    if (data.type === "openclaw_action") {
      // ── OpenClaw action telemetry (file write, delete, wasm_run, etc.) ──
      const action = {
        id: data.action_id || "",
        type: data.action_type || "unknown",
        description: data.description || "",
        status: data.status || "completed",
      };

      // For pending actions, add to the pending list
      if (action.status === "pending" && action.id) {
        pendingActions = [...pendingActions, action];
      } else if (action.id) {
        // Update matching pending action
        pendingActions = pendingActions.filter((a) => a.id !== action.id);
        // Show result as a system message
        const statusIcon =
          action.status === "approved" ? "✅" :
          action.status === "denied" ? "⛔" :
          action.status === "timeout" ? "⏱️" : "ℹ️";
        messages = [...messages, {
          role: "system",
          text: `${statusIcon} OpenClaw action: ${action.type} — ${action.description} [${action.status}]`,
        }];
      }

      // Also append as a system message for visibility
      if (action.status === "pending") {
        messages = [...messages, {
          role: "system",
          text: `🔧 OpenClaw requests: ${action.description}`,
          actionId: action.id,
          actionType: action.type,
          pending: true,
        }];
      }
      sending = false;
      return;
    }

    if (data.type === "chat_reply_chunk") {
      const sid = data.session_id;
      if (!pendingChunks[sid]) {
        pendingChunks[sid] = {
          parts: new Array(data.total),
          total: data.total,
          count: 0,
        };
      }
      const acc = pendingChunks[sid];
      if (acc.parts[data.seq] === undefined) {
        acc.parts[data.seq] = data.text;
        acc.count++;
      }
      // Wait until all chunks arrive, then process as a normal chat_reply
      if (acc.count < acc.total) return;

      const fullText = acc.parts.join("");
      delete pendingChunks[sid];
      // Rewrite data as a synthetic chat_reply for the logic below
      data = { type: "chat_reply", text: fullText, commands: [], session_id: sid };
    }

    if (data.type === "step") {
      // Append a timeline entry to the most recent user message
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        if (!updated.history) updated.history = [];
        updated.status = `step_${data.seq}_of_${data.total}`;
        updated.history = [
          ...updated.history,
          { stage: "step", ts: data.ts, text: data.text, seq: data.seq, total: data.total },
        ];
        messages[lastIdx] = updated;
      }

      // Also show step as a separate message with collapsible detail
      messages = [...messages, {
        role: "step",
        text: data.text,
        seq: data.seq,
        total: data.total,
        ts: data.ts,
      }];
    } else if (data.type === "command_result") {
      // Attach Lua command result to the last bot message as an action
      for (let i = messages.length - 1; i >= 0; i--) {
        if (messages[i].role === "bot") {
          const updated = { ...messages[i] };
          if (!updated.commandResults) updated.commandResults = [];
          updated.commandResults = [...updated.commandResults, {
            script: data.script,
            output: data.output,
            status: data.status,
          }];
          messages[i] = updated;
          break;
        }
      }
    } else if (data.type === "chat_reply") {
      // ── Session reset ack → header badge, not a chat message ──
      if (data.text && data.text.includes("Session reset acknowledged")) {
        sessionAckBadge = true;
        // Auto-clear the badge after 3 s
        setTimeout(() => { sessionAckBadge = false; }, 3000);
        sending = false;
        return;
      }

      // Update the last user message's history to completed
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        updated.status = "completed";
        if (!updated.history) updated.history = [];
        updated.history = [
          ...updated.history,
          { stage: "completed", ts: data.ts },
        ];
        updated.reply = data;
        messages[lastIdx] = updated;
      }

      // Only deduplicate if the previous bot message has the SAME text
      // (e.g. repeated "🤖 Processing…" from interim cloud replies).
      // Different messages (actual reply, step results, etc.)
      // are appended so the user sees the full conversation history.
      const lastMsg = messages.length > 0 ? messages[messages.length - 1] : null;
      const newMsg = {
        role: "bot",
        text: data.text,
        commands: data.commands || [],
        ts: data.ts,
      };

      if (lastMsg && lastMsg.role === "bot" && lastMsg.text === data.text) {
        messages = [...messages.slice(0, -1), newMsg];
      } else {
        messages = [...messages, newMsg];
      }
      sending = false;
    } else if (data.type === "ack") {
      // Update the last user message with acknowledgment
      const lastIdx = messages.length - 1;
      if (lastIdx >= 0 && messages[lastIdx].role === "user") {
        const updated = { ...messages[lastIdx] };
        updated.status = data.status || "sent";
        if (!updated.history) updated.history = [];
        updated.history = [...updated.history, { stage: data.status, ts: Date.now() }];
        messages[lastIdx] = updated;
      }
    } else if (data.type === "error" && data.text) {
      messages = [...messages, { role: "system", text: `⚠️ ${data.text}` }];
      sending = false;
    }
  }

  // ── Send message via WebSocket ───────────────────────────────

  function send() {
    const text = inputText.trim();
    if (!text || sending || !ws || ws.readyState !== WebSocket.OPEN) return;

    // Add user message with timeline
    messages = [...messages, {
      role: "user",
      text,
      status: "sending",
      history: [{ stage: "sending", ts: Date.now() }],
      ts: Date.now(),
    }];
    inputText = "";
    sending = true;
    lastSentTs = Date.now();

    ws.send(JSON.stringify({
      type: "user_chat_input",
      text,
      session_id: sessionId,
      ts: Math.floor(Date.now() / 1000),
    }));
  }

  // ── Respond to a permission request ──────────────────────────

  async function respondPermission(actionId, approved) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    ws.send(JSON.stringify({
      type: "permission_response",
      action_id: actionId,
      approved,
      ts: Math.floor(Date.now() / 1000),
    }));

    // Remove from pending actions
    pendingActions = pendingActions.filter((a) => a.id !== actionId);

    // Update the last system message with the user's decision
    for (let i = messages.length - 1; i >= 0; i--) {
      if (messages[i].actionId === actionId) {
        const updated = { ...messages[i] };
        updated.pending = false;
        updated.text = approved
          ? `✅ Approved: ${messages[i].text.replace("🔧 OpenClaw requests: ", "")}`
          : `⛔ Denied: ${messages[i].text.replace("🔧 OpenClaw requests: ", "")}`;
        messages[i] = updated;
        break;
      }
    }
  }

  // ── New conversation ─────────────────────────────────────────

  function newConversation() {
    // Save the current conversation before clearing
    if (messages.length > 0) {
      saveConversation(sessionId, { messages });
    }
    // Always refresh sidebar so saved convos appear / empty state updates
    sidebarRefreshKey++;

    const newId = generateUUID();
    sessionId = newId;
    messages = [];
    sending = false;

    // Notify the server to discard old context
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: newId,
        ts: Math.floor(Date.now() / 1000),
      }));
    }

    // Focus the input for immediate typing
    inputEl?.focus();
  }

  // Sidebar: manual refresh button
  function refreshSidebar() {
    sidebarRefreshKey++;
  }

  // ── Import callback (called from sidebar) ────────────────────

  function handleImportDone(count) {
    sidebarRefreshKey++;
  }

  // ── Auto-scroll when new messages arrive ────────────────────
  $effect(() => {
    const msgs = messages;
    if (msgs.length > 0 && scrollAnchor) {
      // Use requestAnimationFrame to let the DOM update first
      requestAnimationFrame(() => {
        scrollAnchor.scrollIntoView({ behavior: "smooth", block: "end" });
      });
    }
  });

  // ── Auto-focus the message input ────────────────────────────
  $effect(() => {
    // Re-focus whenever sending becomes false (reply received) or
    // session changes, so the user can keep typing without clicking.
    if (!sending && inputEl && connected) {
      inputEl.focus();
    }
  });

  // ── Persist sessionId on every change ───────────────────────
  $effect(() => {
    const id = sessionId;
    // Sync the current session ID to localStorage so it survives
    // component re-mounts and page refreshes.
    try {
      localStorage.setItem(SESSION_STORAGE_KEY, id);
    } catch { /* localStorage unavailable — silent */ }
  });

  // ── Lifecycle ────────────────────────────────────────────────

  onMount(() => {
    // Restore messages for the last active conversation so the user
    // sees continuity instead of a blank chat after re-mount.
    const convo = getConversation(sessionId);
    if (convo && convo.messages.length > 0) {
      messages = convo.messages;
    }

    connect();
    return () => disconnect();
  });
</script>

<div class="flex flex-row flex-1 min-h-0">
  <!-- Conversation Sidebar -->
  <ConversationSidebar
    activeId={sessionId}
    refreshKey={sidebarRefreshKey}
    onSelect={selectConversation}
    onNew={newConversation}
    onImport={handleImportDone}
    onDelete={(id) => {
      // If deleting the active conversation, start fresh without saving
      if (id === sessionId) {
        const newId = generateUUID();
        sessionId = newId;
        messages = [];
        sending = false;
        if (ws && ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({
            type: "session_reset",
            session_id: newId,
            ts: Math.floor(Date.now() / 1000),
          }));
        }
      }
    }}
  />

  <!-- Main chat area -->
  <div class="flex flex-col flex-1 min-h-0 min-w-0">
    <!-- Header -->
    <header class="flex items-center justify-between gap-2 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20 shrink-0">
      <div class="flex items-center gap-3">
        <button onclick={() => navigate("home")}
                class="text-lg hover:text-mpx-orange transition-colors cursor-pointer">‹</button>
        <h2 class="font-semibold">Chat</h2>
      </div>
      <div class="flex items-center gap-2">
        <!-- Sidebar refresh button -->
        <button onclick={refreshSidebar}
                class="text-xs text-mpx-muted hover:text-mpx-orange transition-colors cursor-pointer px-2 py-1 rounded border border-mpx-muted/20 hover:border-mpx-orange/50"
                title="Refresh conversation list">
          &#x21bb;
        </button>

        <button onclick={newConversation}
                class="text-xs text-mpx-muted hover:text-mpx-orange transition-colors cursor-pointer px-2 py-1 rounded border border-mpx-muted/20 hover:border-mpx-orange/50">
          + New
        </button>

        <!-- Status: connection + session-ack badge -->
        <span class="flex items-center gap-1.5 text-xs">
          {#if sessionAckBadge}
            <span class="flex items-center gap-1 text-mpx-orange-light">
              <span class="w-2 h-2 rounded-full bg-mpx-orange-light"></span>
              Session reset
            </span>
          {:else}
            <span class="flex items-center gap-1.5 text-xs {connected ? 'text-green-400' : 'text-red-400'}">
              <span class="w-2 h-2 rounded-full {connected ? 'bg-green-400' : 'bg-red-400'}"></span>
              {sending ? "Sending…" : connected ? "Online" : "Offline"}
            </span>
          {/if}
        </span>
      </div>
    </header>

    <!-- Messages -->
    <div class="flex-1 overflow-y-auto min-h-0 px-4 py-3 space-y-3">
    {#if messages.length === 0}
      <p class="text-center text-mpx-muted text-sm mt-8">
        No messages yet. Type something below.
      </p>
    {/if}

    {#each messages as msg, i}
      <!-- Date delineator: always show on first message, or between messages on different days -->
      {#if i === 0 || (i > 0 && isNewDay(findPrevTimestampMsg(messages, i - 1), msg))}
        <div class="flex justify-center my-3">
          <span class="text-[11px] text-mpx-muted bg-mpx-surface/60 px-3 py-1 rounded-full border border-mpx-muted/10">
            {formatDateLabel(msg)}
          </span>
        </div>
      {/if}
      {#if msg.role === "step"}
        <!-- Step message — progress indicator -->
        <div class="flex justify-start">
          <div class="max-w-[85%] rounded-xl px-4 py-2.5 text-sm bg-mpx-surface/40 border-l-4 border-mpx-orange">
            <div class="flex items-center gap-2">
              <span class="flex h-5 w-5 shrink-0 items-center justify-center rounded-full bg-mpx-orange/20 text-xs text-mpx-orange">
                {msg.seq}
              </span>
              <span class="text-xs text-mpx-muted">Step {msg.seq} of {msg.total}</span>
            </div>
            <p class="mt-1 text-mpx-text">{msg.text}</p>
          </div>
        </div>
      {:else if msg.role === "user"}
        <!-- User message with timeline -->
        <div class="flex justify-end">
          <div class="max-w-[80%]">
            <div class="rounded-xl px-4 py-2 text-sm bg-mpx-orange text-white rounded-br-sm">
              {msg.text}
              {#if msg.ts}
                <div class="text-[9px] text-white/40 text-right mt-0.5">{formatTimestamp(msg.ts)}</div>
              {/if}
            </div>
            <!-- Timeline indicator (one-line status + expand) -->
            {#if msg.history && msg.history.length > 0}
              {@const lastEntry = msg.history[msg.history.length - 1]}
              <div class="mt-1">
                {#if msg.history.length === 1}
                  <!-- Single entry — plain status line -->
                  <div class="flex items-center gap-1.5 text-xs text-mpx-muted">
                    {#if lastEntry.stage === "sending"}
                      <span class="w-1.5 h-1.5 rounded-full bg-yellow-400 shrink-0"></span>
                      <span>Sending…</span>
                    {:else if lastEntry.stage === "sent"}
                      <span class="w-1.5 h-1.5 rounded-full bg-blue-400 shrink-0"></span>
                      <span>Sent</span>
                    {:else if lastEntry.stage === "relayed"}
                      <span class="w-1.5 h-1.5 rounded-full bg-blue-400 shrink-0"></span>
                      <span>Relayed to cloud</span>
                    {:else if lastEntry.stage === "processing"}
                      <span class="w-1.5 h-1.5 rounded-full bg-purple-400 shrink-0"></span>
                      <span>AI processing…</span>
                    {:else if lastEntry.stage === "step"}
                      <span class="w-1.5 h-1.5 rounded-full bg-mpx-orange shrink-0"></span>
                      <span>Step {lastEntry.seq}/{lastEntry.total}: {lastEntry.text}</span>
                    {:else if lastEntry.stage === "completed"}
                      <span class="w-1.5 h-1.5 rounded-full bg-green-400 shrink-0"></span>
                      <span class="text-green-400">✓ Complete</span>
                    {/if}
                  </div>
                {:else}
                  <!-- Multiple entries — summary has status + arrow+count on one line -->
                  <details class="group">
                    <summary class="flex items-center gap-1.5 text-xs text-mpx-muted list-none cursor-pointer">
                      {#if lastEntry.stage === "sending"}
                        <span class="w-1.5 h-1.5 rounded-full bg-yellow-400 shrink-0"></span>
                        <span>Sending…</span>
                      {:else if lastEntry.stage === "sent"}
                        <span class="w-1.5 h-1.5 rounded-full bg-blue-400 shrink-0"></span>
                        <span>Sent</span>
                      {:else if lastEntry.stage === "relayed"}
                        <span class="w-1.5 h-1.5 rounded-full bg-blue-400 shrink-0"></span>
                        <span>Relayed to cloud</span>
                      {:else if lastEntry.stage === "processing"}
                        <span class="w-1.5 h-1.5 rounded-full bg-purple-400 shrink-0"></span>
                        <span>AI processing…</span>
                      {:else if lastEntry.stage === "step"}
                        <span class="w-1.5 h-1.5 rounded-full bg-mpx-orange shrink-0"></span>
                        <span>Step {lastEntry.seq}/{lastEntry.total}: {lastEntry.text}</span>
                      {:else if lastEntry.stage === "completed"}
                        <span class="w-1.5 h-1.5 rounded-full bg-green-400 shrink-0"></span>
                        <span class="text-green-400">✓ Complete</span>
                      {/if}
                      <span class="text-mpx-muted/40 hover:text-mpx-orange transition-colors flex items-center gap-0.5">
                        <span class="inline-block transition-transform group-open:rotate-90 text-[10px]">▶</span>
                        <span class="text-[11px]">{msg.history.length - 1}</span>
                      </span>
                    </summary>
                    <div class="mt-0.5 space-y-0.5">
                      {#each [...msg.history].reverse().slice(1) as entry}
                        <div class="flex items-center gap-1.5 text-xs text-mpx-muted/70">
                          {#if entry.stage === "sending"}
                            <span class="w-1.5 h-1.5 rounded-full bg-yellow-400/60"></span>
                            <span>Sending…</span>
                          {:else if entry.stage === "sent"}
                            <span class="w-1.5 h-1.5 rounded-full bg-blue-400/60"></span>
                            <span>Sent</span>
                          {:else if entry.stage === "relayed"}
                            <span class="w-1.5 h-1.5 rounded-full bg-blue-400/60"></span>
                            <span>Relayed to cloud</span>
                          {:else if entry.stage === "processing"}
                            <span class="w-1.5 h-1.5 rounded-full bg-purple-400/60"></span>
                            <span>AI processing…</span>
                          {:else if entry.stage === "step"}
                            <span class="w-1.5 h-1.5 rounded-full bg-mpx-orange/60"></span>
                            <span>Step {entry.seq}/{entry.total}: {entry.text}</span>
                          {:else if entry.stage === "completed"}
                            <span class="w-1.5 h-1.5 rounded-full bg-green-400/60"></span>
                            <span class="text-green-400/70">✓ Complete</span>
                          {/if}
                        </div>
                      {/each}
                    </div>
                  </details>
                {/if}
              </div>
            {/if}
          </div>
        </div>
      {:else if msg.role === "bot"}
        <!-- Bot reply with Markdown rendering + expandable Lua commands -->
        <div class="flex justify-start">
          <div class="max-w-[80%]">
            <div class="prose prose-sm prose-invert max-w-none rounded-xl px-4 py-2 bg-mpx-surface text-mpx-text rounded-bl-sm markdown-body">
              {@html renderMarkdown(msg.text)}
              {#if msg.ts}
                <div class="text-[9px] text-mpx-muted/40 text-right mt-0.5">{formatTimestamp(msg.ts)}</div>
              {/if}
            </div>
            {#if msg.commands && msg.commands.length > 0}
              <details class="mt-1">
                <summary class="text-xs text-mpx-muted cursor-pointer hover:text-mpx-orange transition-colors ml-1">
                  ▶ {msg.commands.length} Lua {msg.commands.length === 1 ? "command" : "commands"}
                </summary>
                <div class="mt-1 space-y-1">
                  {#each msg.commands as cmd, i}
                    <div class="rounded bg-black/30 p-2 border-l-2 border-mpx-orange/60">
                      <div class="flex items-center gap-2 mb-1">
                        <span class="text-xs text-mpx-muted">#{i + 1}</span>
                        <span class="text-[10px] px-1.5 py-0.5 rounded bg-mpx-orange/20 text-mpx-orange uppercase tracking-wide">{cmd.type}</span>
                      </div>
                      <pre class="text-[11px] text-mpx-text/90 overflow-x-auto whitespace-pre-wrap font-mono">{cmd.script}</pre>
                    </div>
                  {/each}
                </div>
              </details>
            {/if}
            {#if msg.commandResults && msg.commandResults.length > 0}
              <div class="mt-1 space-y-1">
                {#each msg.commandResults as result}
                  <!-- svelte-ignore a11y_no_static_element_interactions -->
                  <div class="rounded bg-black/30 p-2 border-l-2 {result.status === 'completed' ? 'border-green-500' : result.status === 'error' ? 'border-red-500' : 'border-yellow-500'}">
                    <div class="flex items-center gap-1.5 mb-1">
                      <span class="text-xs">
                        {#if result.status === "completed"}<span class="text-green-400">✓</span>
                        {:else if result.status === "error"}<span class="text-red-400">✗</span>
                        {:else if result.status === "timeout"}<span class="text-yellow-400">⏱</span>
                        {/if}
                      </span>
                      <span class="text-[10px] px-1.5 py-0.5 rounded uppercase tracking-wide {result.status === 'completed' ? 'bg-green-400/20 text-green-400' : result.status === 'error' ? 'bg-red-400/20 text-red-400' : 'bg-yellow-400/20 text-yellow-400'}">
                        {result.status}
                      </span>
                    </div>
                    {#if result.output}
                      <pre class="text-[11px] text-green-300/90 overflow-x-auto whitespace-pre-wrap font-mono">{result.output}</pre>
                    {/if}
                  </div>
                {/each}
              </div>
            {/if}
          </div>
        </div>
      {:else if msg.role === "system"}
        <!-- System message -->
        <div class="flex justify-center">
          <div class="max-w-[90%] rounded-xl px-4 py-2 text-xs bg-mpx-bg text-mpx-muted italic border border-mpx-muted/10 text-center">
            {msg.text}
          </div>
          {#if msg.pending && msg.actionId}
            <div class="flex justify-center gap-2 mt-1">
              <button onclick={() => respondPermission(msg.actionId, true)}
                      class="px-3 py-1 rounded text-xs bg-emerald-700/60 text-emerald-200 hover:bg-emerald-700/80 border border-emerald-600/50 cursor-pointer transition-colors">
                ✅ Approve
              </button>
              <button onclick={() => respondPermission(msg.actionId, false)}
                      class="px-3 py-1 rounded text-xs bg-red-700/60 text-red-200 hover:bg-red-700/80 border border-red-600/50 cursor-pointer transition-colors">
                ⛔ Deny
              </button>
            </div>
          {/if}
        </div>
      {/if}
    {/each}

    <!-- Live response throbber + counter (left side while waiting) -->
    {#if sending && lastSentTs}
      <div class="flex justify-start">
        <div class="flex items-center gap-3 px-4 py-3 rounded-xl bg-mpx-surface/30 border border-mpx-muted/10">
          <span class="w-5 h-5 border-2 border-mpx-orange/60 border-t-mpx-orange rounded-full animate-spin"></span>
          <span class="text-base font-mono text-mpx-muted tabular-nums">{Math.floor((now - lastSentTs) / 1000)}s</span>
        </div>
      </div>
    {/if}

    <!-- Scroll anchor for auto-scroll -->
    <div bind:this={scrollAnchor}></div>
  </div>

  <!-- Input (tabindex=0 so it's the first tabbable element) -->
  <div class="flex items-center gap-2 px-4 py-3 bg-mpx-surface border-t border-mpx-muted/20 shrink-0">
    <input
      bind:this={inputEl}
      bind:value={inputText}
      onkeydown={(e) => e.key === "Enter" && send()}
      placeholder={connected ? "Type a message…" : "Reconnecting…"}
      disabled={sending || !connected}
      tabindex="0"
      class="flex-1 rounded-lg bg-mpx-bg border border-mpx-muted/20 px-3 py-2
             text-sm text-mpx-text outline-none focus:border-mpx-orange/50
             disabled:opacity-40 disabled:cursor-not-allowed"
    />
    <button onclick={send}
            disabled={!inputText.trim() || sending || !connected}
            class="rounded-lg bg-mpx-orange px-4 py-2 text-sm text-white
                   hover:bg-mpx-orange-light transition-colors cursor-pointer
                   disabled:opacity-40 disabled:cursor-not-allowed
                   {!inputText.trim() || sending || !connected ? 'opacity-50' : ''}">
      {sending ? "…" : "Send"}
    </button>
  </div>
  </div>
</div>
