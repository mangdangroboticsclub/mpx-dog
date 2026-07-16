<script>
  import { marked } from "marked";

  let { msg, isFirst = false, isLastInGroup = false } = $props();

  /* ── Markdown renderer ─────────────────── */
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

  /** Format timestamp */
  function fmtTime(ts) {
    if (!ts) return "";
    const d = ts < 100000000000 ? new Date(ts * 1000) : new Date(ts);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  let isUser = $derived(msg.role === "user");
  let isBot = $derived(msg.role === "bot");
  let isSystem = $derived(msg.role === "system");
  let isStep = $derived(msg.role === "step");
</script>

{#if isStep}
  <!-- Step message -->
  <div class="msg-row msg-row-step">
    <div class="step-bubble">
      <div class="step-header">
        <span class="step-number">{msg.seq}</span>
        <span class="step-label">Step {msg.seq} of {msg.total}</span>
      </div>
      <p class="step-text">{msg.text}</p>
    </div>
  </div>
{:else if isSystem}
  <!-- System message -->
  <div class="msg-row msg-row-center">
    <div class="system-bubble">
      {#if msg.pending && msg.actionId}
        <p>{msg.text}</p>
        <div class="action-btns">
          <button class="approve-btn" onclick={() => respondPermission(msg.actionId, true)}>✅ Approve</button>
          <button class="deny-btn" onclick={() => respondPermission(msg.actionId, false)}>⛔ Deny</button>
        </div>
      {:else}
        <p>{msg.text}</p>
      {/if}
    </div>
  </div>
{:else if isUser}
  <!-- User message -->
  <div class="msg-row msg-row-user">
    <div class="user-bubble">
      <p class="user-text">{msg.text}</p>
      <div class="user-meta">
        <span class="msg-time">{fmtTime(msg.ts)}</span>
        {#if msg.status === "sending"}
          <span class="sending-dot"></span>
        {:else if msg.status === "completed"}
          <span class="check-icon">✓</span>
        {/if}
      </div>
    </div>
  </div>
{:else if isBot}
  <!-- Bot message -->
  <div class="msg-row msg-row-bot">
    <div class="bot-bubble">
      <div class="bot-content markdown-body">
        {@html renderMarkdown(msg.text)}
      </div>
      {#if msg.ts}
        <div class="bot-meta">
          <span class="msg-time">{fmtTime(msg.ts)}</span>
          {#if msg.commands?.length}
            <span class="cmd-badge">{msg.commands.length} cmd</span>
          {/if}
        </div>
      {/if}
    </div>
    {#if msg.commandResults?.length}
      <div class="cmd-results">
        {#each msg.commandResults as result}
          <div class="cmd-result" class:completed={result.status === 'completed'} class:error={result.status === 'error'}>
            <span class="cmd-status">
              {#if result.status === "completed"}✓
              {:else if result.status === "error"}✗
              {:else}⏱{/if}
            </span>
            {#if result.output}
              <pre class="cmd-output">{result.output}</pre>
            {/if}
          </div>
        {/each}
      </div>
    {/if}
  </div>
{/if}

<style>
  .msg-row {
    display: flex;
    padding: 2px 16px;
  }
  .msg-row-step {
    justify-content: flex-start;
  }
  .msg-row-center {
    justify-content: center;
  }
  .msg-row-user {
    justify-content: flex-end;
  }
  .msg-row-bot {
    justify-content: flex-start;
  }

  /* ── Step bubble ─────────────────────── */
  .step-bubble {
    max-width: 85%;
    border-radius: 12px;
    padding: 10px 14px;
    background: #f5f5f5;
    border-left: 3px solid #FFE605;
  }
  .step-header {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 4px;
  }
  .step-number {
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: rgba(255, 230, 5, 0.25);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 0.65rem;
    font-weight: 700;
    color: #b89e00;
  }
  .step-label {
    font-size: 0.7rem;
    color: #999;
  }
  .step-text {
    font-size: 0.8rem;
    color: #555;
  }

  /* ── System bubble ───────────────────── */
  .system-bubble {
    max-width: 90%;
    border-radius: 12px;
    padding: 8px 14px;
    background: #fafafa;
    border: 1px solid #eee;
    text-align: center;
    font-size: 0.75rem;
    color: #999;
    font-style: italic;
  }
  .action-btns {
    display: flex;
    justify-content: center;
    gap: 8px;
    margin-top: 8px;
  }
  .approve-btn {
    padding: 6px 14px;
    border: 1px solid #4ade80;
    border-radius: 8px;
    background: rgba(74, 222, 128, 0.1);
    color: #22c55e;
    font-size: 0.75rem;
    cursor: pointer;
    transition: background 0.12s;
  }
  .approve-btn:hover {
    background: rgba(74, 222, 128, 0.2);
  }
  .deny-btn {
    padding: 6px 14px;
    border: 1px solid #f87171;
    border-radius: 8px;
    background: rgba(248, 113, 113, 0.1);
    color: #ef4444;
    font-size: 0.75rem;
    cursor: pointer;
    transition: background 0.12s;
  }
  .deny-btn:hover {
    background: rgba(248, 113, 113, 0.2);
  }

  /* ── User bubble ─────────────────────── */
  .user-bubble {
    background: #FFE605;
    border-radius: 16px 16px 4px 16px;
    padding: 10px 14px;
    max-width: 80%;
  }
  .user-text {
    font-size: 0.85rem;
    color: #000;
    line-height: 1.4;
    word-wrap: break-word;
  }
  .user-meta {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 4px;
    margin-top: 4px;
  }
  .msg-time {
    font-size: 0.6rem;
    color: rgba(0, 0, 0, 0.35);
  }
  .sending-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: rgba(0, 0, 0, 0.3);
    animation: pulse 1.2s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.3; }
  }
  .check-icon {
    font-size: 0.6rem;
    color: rgba(0, 0, 0, 0.4);
  }

  /* ── Bot bubble ──────────────────────── */
  .bot-bubble {
    background: #fff;
    border: 1px solid #e8e8e8;
    border-radius: 16px 16px 16px 4px;
    padding: 12px 14px;
    max-width: 85%;
    box-shadow: 0 1px 3px rgba(0,0,0,0.04);
  }
  .bot-content {
    font-size: 0.85rem;
    color: #333;
    line-height: 1.5;
  }
  .bot-meta {
    display: flex;
    align-items: center;
    justify-content: flex-start;
    gap: 8px;
    margin-top: 6px;
  }
  .cmd-badge {
    font-size: 0.6rem;
    padding: 2px 6px;
    border-radius: 4px;
    background: #f0f0f0;
    color: #888;
  }

  /* ── Command results ─────────────────── */
  .cmd-results {
    margin-top: 4px;
    width: 100%;
  }
  .cmd-result {
    border-radius: 8px;
    padding: 8px 12px;
    margin-bottom: 4px;
    border-left: 3px solid;
  }
  .cmd-result.completed {
    background: #f0fdf4;
    border-color: #22c55e;
  }
  .cmd-result.error {
    background: #fef2f2;
    border-color: #ef4444;
  }
  .cmd-status {
    font-size: 0.7rem;
    font-weight: 600;
  }
  .cmd-result.completed .cmd-status { color: #22c55e; }
  .cmd-result.error .cmd-status { color: #ef4444; }
  .cmd-output {
    font-family: 'JetBrains Mono', monospace;
    font-size: 0.7rem;
    color: #555;
    margin-top: 4px;
    white-space: pre-wrap;
    overflow-x: auto;
  }

  :global(.msg-link) {
    color: #b89e00;
    text-decoration: underline;
  }
</style>
