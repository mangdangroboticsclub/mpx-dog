<script>
  import { onMount } from "svelte";
  import { marked } from "marked";
  import ChatSidebar from "./ChatSidebar.svelte";
  import ChatMessage from "./ChatMessage.svelte";
  import EmptyChat from "./EmptyChat.svelte";
  import {
    saveConversation,
    getConversation,
    generateUUID,
    listConversations,
  } from "../conversationStore.js";

  let { navigate } = $props();

  // ── Session ID persistence ────────────────
  const SESSION_KEY = "mpx_active_session_id";
  function restoreSessionId() {
    try {
      const saved = localStorage.getItem(SESSION_KEY);
      if (saved) return saved;
    } catch {}
    const all = listConversations();
    if (all.length > 0) return all[0].id;
    return generateUUID();
  }

  // ── State ──────────────────────────────────
  let ws = null;
  let connected = $state(false);
  let sessionId = $state(restoreSessionId());
  let messages = $state([]);
  let inputText = $state("");
  let sending = $state(false);
  let showSidebar = $state(false);
  let sidebarRefreshKey = $state(0);
  let scrollAnchor = $state(null);
  let inputEl = $state(null);

  // Live timer for response counter
  let now = $state(Date.now());
  let lastSentTs = $state(null);
  $effect(() => {
    const id = setInterval(() => { now = Date.now(); }, 200);
    return () => clearInterval(id);
  });

  // Accumulator for chunked chat_reply reassembly
  let pendingChunks = {};

  // ── Markdown renderer ──────────────────────
  const renderer = new marked.Renderer();
  renderer.link = ({ href, text }) =>
    `<a href="${href}" target="_blank" rel="noopener noreferrer" class="msg-link">${text}</a>`;
  marked.setOptions({ renderer, breaks: true, gfm: true });

  function renderMarkdown(text) {
    if (!text) return "";
    const raw = marked.parse(text, { async: false });
    return raw
      .replace(/<script[\s\S]*?<\/script>/gi, "")
      .replace(/\son\w+\s*=\s*["']?[^"'\s>]+["']?/gi, "");
  }

  // ── Timestamp helpers ─────────────────────
  function normalizeTs(ts) { return ts < 100000000000 ? ts * 1000 : ts; }

  // ── Conversation persistence ──────────────
  let debounceTimer;
  function persist() {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      if (messages.length > 0) {
        saveConversation(sessionId, { messages });
        sidebarRefreshKey++;
      }
    }, 500);
  }

  $effect(() => {
    const msgs = messages;
    if (msgs.length > 0) persist();
  });

  $effect(() => {
    try { localStorage.setItem(SESSION_KEY, sessionId); } catch {}
  });

  // ── Load conversation ─────────────────────
  function loadConversation(id) {
    const convo = getConversation(id);
    if (convo) {
      sessionId = convo.id;
      messages = convo.messages || [];
    }
  }

  $effect(() => { loadConversation(sessionId); });

  // ── Conversation actions ──────────────────
  function selectConversation(id) {
    if (messages.length > 0) saveConversation(sessionId, { messages });
    sidebarRefreshKey++;
    sessionId = id;
    messages = getConversation(id)?.messages || [];
    sending = false;
    if (window.innerWidth < 768) showSidebar = false;
    // Tell server about session change
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "session_reset",
        session_id: id,
        ts: Math.floor(Date.now() / 1000),
      }));
    }
  }

  function newConversation() {
    if (messages.length > 0) saveConversation(sessionId, { messages });
    sidebarRefreshKey++;
    sessionId = generateUUID();
    messages = [];
    sending = false;
  }

  function handleDelete(id) {
    if (id === sessionId) {
      sessionId = generateUUID();
      messages = [];
      sending = false;
    }
  }

  // ═══════════════════════════════════════════
  //  WebSocket — Real chat integration
  // ═══════════════════════════════════════════
  const WS_URL = `ws://${location.host}/v1/chat/ui`;

  function connect() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

    const socket = new WebSocket(WS_URL);

    socket.onopen = () => {
      connected = true;
      socket.send(JSON.stringify({
        type: "session_reset",
        session_id: sessionId,
        ts: Math.floor(Date.now() / 1000),
      }));
    };

    socket.onclose = () => {
      connected = false;
      ws = null;
      setTimeout(() => {
        if (document.visibilityState !== "hidden") connect();
      }, 3000);
    };

    socket.onerror = () => { connected = false; };

    socket.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        handleMessage(data);
      } catch {}
    };

    ws = socket;
  }

  function disconnect() {
    if (ws) {
      ws.onclose = null;
      ws.close();
      ws = null;
    }
    connected = false;
  }

  function handleMessage(data) {
    switch (data.type) {
      case "chat_reply": {
        const botMsg = {
          role: "bot",
          text: data.text || "",
          ts: data.ts || Date.now(),
          commands: data.commands || [],
          commandResults: data.command_results || [],
        };
        messages = [...messages, botMsg];
        sending = false;
        break;
      }
      case "chat_chunk": {
        // Accumulate chunked responses
        const sid = data.session_id || sessionId;
        if (!pendingChunks[sid]) pendingChunks[sid] = "";
        pendingChunks[sid] += data.text || "";
        if (data.final) {
          const fullText = pendingChunks[sid];
          delete pendingChunks[sid];
          messages = [...messages, {
            role: "bot",
            text: fullText,
            ts: data.ts || Date.now(),
            commands: data.commands || [],
            commandResults: data.command_results || [],
          }];
          sending = false;
        }
        break;
      }
      case "step": {
        messages = [...messages, {
          role: "step",
          text: data.text || "",
          seq: data.seq,
          total: data.total,
          ts: data.ts,
        }];
        break;
      }
      case "system": {
        messages = [...messages, {
          role: "system",
          text: data.text || "",
          ts: data.ts,
        }];
        break;
      }
      case "session_ack": {
        // Server acknowledged our session
        break;
      }
      case "error": {
        messages = [...messages, {
          role: "system",
          text: `⚠️ ${data.text || "An error occurred"}`,
          ts: Date.now(),
        }];
        sending = false;
        break;
      }
    }
  }

  // ── Send message ──────────────────────────
  function sendMessage(text) {
    if (!text.trim() || sending) return;
    if (!connected) return;

    const userTs = Date.now();
    const userMsg = {
      role: "user",
      text: text.trim(),
      status: "completed",
      ts: userTs,
    };
    messages = [...messages, userMsg];
    inputText = "";
    sending = true;
    lastSentTs = userTs;

    ws.send(JSON.stringify({
      type: "chat",
      session_id: sessionId,
      text: text.trim(),
      ts: Math.floor(userTs / 1000),
    }));
  }

  // ── Connect on mount ──────────────────────
  onMount(() => {
    connect();
    return () => disconnect();
  });

  // Reconnect on visibility change
  $effect(() => {
    function onVisible() {
      if (document.visibilityState === "visible" && !connected) connect();
    }
    document.addEventListener("visibilitychange", onVisible);
    return () => document.removeEventListener("visibilitychange", onVisible);
  });

  // ── Auto-scroll ───────────────────────────
  $effect(() => {
    if (messages.length > 0 && scrollAnchor) {
      requestAnimationFrame(() => {
        scrollAnchor.scrollIntoView({ behavior: "smooth", block: "end" });
      });
    }
  });
</script>

<div class="chat-root">
  <!-- Sidebar (slide animation) -->
  <div class="sidebar-backdrop" class:visible={showSidebar} onclick={() => (showSidebar = false)}></div>
  <div class="sidebar-panel" class:open={showSidebar}>
    <ChatSidebar
      activeId={sessionId}
      refreshKey={sidebarRefreshKey}
      onSelect={selectConversation}
      onNew={newConversation}
      onDelete={handleDelete}
    />
  </div>

  <!-- Main chat area -->
  <div class="chat-main">
    <!-- Action bar (under header) -->
    <div class="action-bar">
      <button
        class="action-bar-btn"
        class:active={showSidebar}
        onclick={() => (showSidebar = !showSidebar)}
        aria-label="Conversation history"
        title="Conversation history"
      >
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="12" cy="12" r="10"/>
          <polyline points="12 6 12 12 16 14"/>
        </svg>
        <span>History</span>
      </button>
      <button
        class="action-bar-btn"
        onclick={newConversation}
        aria-label="New conversation"
        title="New conversation"
      >
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <line x1="12" y1="5" x2="12" y2="19"/>
          <line x1="5" y1="12" x2="19" y2="12"/>
        </svg>
        <span>New</span>
      </button>
      <span class="connection-dot" class:connected class:disconnected={!connected} title={connected ? "Connected" : "Disconnected"}></span>
    </div>

    <!-- Messages or Empty state -->
    {#if messages.length === 0 && !sending}
      <EmptyChat />
    {:else}
      <div class="messages-area">
        <div class="messages-scroll">
          {#each messages as msg, i}
            <ChatMessage
              {msg}
              isFirst={i === 0}
              isLastInGroup={i === messages.length - 1 || messages[i + 1]?.role !== msg.role}
            />
          {/each}

          <!-- Sending indicator -->
          {#if sending && lastSentTs}
            <div class="sending-indicator">
              <span class="spinner"></span>
              <span class="timer">{Math.floor((now - lastSentTs) / 1000)}s</span>
            </div>
          {/if}

          <div bind:this={scrollAnchor}></div>
        </div>
      </div>
    {/if}

    <!-- Input bar (always visible) -->
    <div class="input-bar">
      <div class="input-row">
        <input
          bind:value={inputText}
          onkeydown={(e) => e.key === "Enter" && sendMessage(inputText)}
          placeholder="Start typing here."
          disabled={sending}
          class="chat-input"
        />
        <button
          class="send-btn"
          onclick={() => sendMessage(inputText)}
          disabled={!inputText.trim() || sending}
        >
          {#if sending}
            <span class="spinner small"></span>
          {:else}
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <line x1="22" y1="2" x2="11" y2="13"/>
              <polygon points="22 2 15 22 11 13 2 9 22 2"/>
            </svg>
          {/if}
        </button>
      </div>
    </div>
  </div>
</div>

<style>
  .chat-root {
    display: flex;
    height: 100%;
    background: #f8f8f8;
    position: relative;
  }

  /* ── Sidebar (slide) ──────────────────── */
  .sidebar-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0,0,0,0.3);
    z-index: 40;
    opacity: 0;
    pointer-events: none;
    transition: opacity 0.3s ease;
  }
  .sidebar-backdrop.visible {
    opacity: 1;
    pointer-events: auto;
  }

  .sidebar-panel {
    position: fixed;
    top: 0;
    left: 0;
    bottom: 0;
    width: 280px;
    z-index: 50;
    transform: translateX(-100%);
    transition: transform 0.3s cubic-bezier(0.22, 1, 0.36, 1);
    box-shadow: 4px 0 12px rgba(0,0,0,0.08);
  }
  .sidebar-panel.open {
    transform: translateX(0);
  }

  /* ── Main chat area ──────────────────── */
  .chat-main {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-width: 0;
  }

  /* ── Action bar (toolbar under header) ─ */
  .action-bar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 14px;
    border-bottom: 1px solid #eee;
    background: #f8f8f8;
    flex-shrink: 0;
  }

  .action-bar-btn {
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 6px 12px;
    border: 1px solid #e0e0e0;
    border-radius: 8px;
    background: #fff;
    color: #666;
    font-size: 0.75rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s;
  }
  .action-bar-btn:hover {
    border-color: #FFE605;
    color: #333;
    background: #fffef5;
  }
  .action-bar-btn.active {
    border-color: #FFE605;
    background: #fffde0;
    color: #333;
  }

  .connection-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-left: auto;
    flex-shrink: 0;
  }
  .connection-dot.connected { background: #22c55e; }
  .connection-dot.disconnected { background: #ef4444; }

  /* ── Messages ────────────────────────── */
  .messages-area {
    flex: 1;
    overflow-y: auto;
    min-height: 0;
  }

  .messages-scroll {
    padding: 12px 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .sending-indicator {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 16px;
    margin: 4px 16px;
    border-radius: 12px;
    background: #f5f5f5;
    border: 1px solid #eee;
    width: fit-content;
  }

  .spinner {
    width: 18px;
    height: 18px;
    border: 2.5px solid rgba(255, 230, 5, 0.4);
    border-top-color: #FFE605;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  .spinner.small {
    width: 16px;
    height: 16px;
    border-width: 2px;
  }
  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .timer {
    font-size: 0.8rem;
    font-weight: 600;
    color: #999;
    font-variant-numeric: tabular-nums;
  }

  /* ── Input bar ───────────────────────── */
  .input-bar {
    padding: 10px 14px 16px;
    border-top: 1px solid #eee;
    background: #f8f8f8;
    flex-shrink: 0;
  }

  .input-row {
    display: flex;
    align-items: center;
    gap: 8px;
    background: #f0f0f0;
    border-radius: 100px;
    padding: 4px 4px 4px 18px;
  }

  .chat-input {
    flex: 1;
    border: none;
    outline: none;
    background: transparent;
    font-size: 0.9rem;
    color: #333;
    padding: 10px 0;
    min-width: 0;
  }
  .chat-input::placeholder {
    color: #bbb;
  }
  .chat-input:disabled {
    opacity: 0.5;
  }

  .send-btn {
    width: 40px;
    height: 40px;
    border-radius: 50%;
    border: none;
    background: #222;
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s;
    flex-shrink: 0;
  }
  .send-btn:hover {
    background: #444;
  }
  .send-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }
</style>
