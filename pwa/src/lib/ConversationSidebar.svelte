<script>
  import {
    listConversations,
    deleteConversation,
    exportConversations,
    importConversations,
  } from "./conversationStore.js";

  let {
    activeId,
    onSelect,
    onNew,
    /** Callback when the active conversation is deleted — parent starts fresh without re-saving */
    onDelete,
    /** Callback fired after import completes — receives imported session IDs */
    onImport,
    /** Bump this number to trigger a list refresh from the parent */
    refreshKey = 0,
  } = $props();

  let conversations = $state([]);
  let sidebarOpen = $state(false);
  let searchText = $state("");

  // ── Refresh list ─────────────────────────────────────────────

  function refresh() {
    conversations = listConversations();
  }

  // Refresh on mount and whenever refreshKey changes
  $effect(() => {
    refreshKey; // 👈 read the prop so Svelte tracks it as a dependency
    refresh();
  });
  // Also refresh when the sidebar opens (mobile drawer)
  $effect(() => {
    if (sidebarOpen) refresh();
  });

  // ── Actions ──────────────────────────────────────────────────

  function handleSelect(id) {
    onSelect?.(id);
    // On mobile, close sidebar after selecting
    if (window.innerWidth < 768) sidebarOpen = false;
  }

  function handleDelete(id, e) {
    e.stopPropagation();
    deleteConversation(id);
    refresh();
    // If the active conversation was deleted, let parent handle cleanup
    if (activeId === id) onDelete?.(id);
  }

  function handleExport() {
    const json = exportConversations();
    const blob = new Blob([json], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `mpx-chat-export-${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }

  function handleImport() {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".json";
    input.onchange = async () => {
      const file = input.files?.[0];
      if (!file) return;
      try {
        const text = await file.text();
        const count = importConversations(text);
        refresh();
        onImport?.(count);
        alert(`Imported ${count} conversation${count === 1 ? "" : "s"}.`);
      } catch (err) {
        alert(`Import failed: ${err.message}`);
      }
    };
    input.click();
  }

  function formatTime(ts) {
    const d = new Date(ts);
    const now = new Date();
    const diffMs = now - d;
    const diffDays = Math.floor(diffMs / 86400000);

    if (diffDays === 0) {
      return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    }
    if (diffDays === 1) return "Yesterday";
    if (diffDays < 7) return `${diffDays}d ago`;
    return d.toLocaleDateString([], { month: "short", day: "numeric" });
  }

  // ── Filtered list ────────────────────────────────────────────

  let filtered = $derived(
    searchText.trim()
      ? conversations.filter((c) =>
          c.title.toLowerCase().includes(searchText.toLowerCase())
        )
      : conversations
  );
</script>

<!-- Toggle button (visible when sidebar is closed) -->
<button
  onclick={() => (sidebarOpen = true)}
  class="md:hidden fixed left-2 top-1/2 -translate-y-1/2 z-30
         w-8 h-12 rounded-r-lg bg-mpx-surface border border-mpx-muted/20 border-l-0
         flex items-center justify-center text-mpx-muted hover:text-mpx-text
         cursor-pointer transition-colors shadow-md"
  aria-label="Open conversation list"
>
  <svg class="w-4 h-4" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
    <path d="M9 18l6-6-6-6" />
  </svg>
</button>

<!-- Sidebar backdrop (mobile) -->
{#if sidebarOpen}
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div
    class="fixed inset-0 z-40 bg-black/40 md:hidden"
    onclick={() => (sidebarOpen = false)}
  ></div>
{/if}

<!-- Sidebar panel -->
<aside
  class="
    {sidebarOpen ? 'translate-x-0' : '-translate-x-full md:translate-x-0'}
    fixed md:sticky top-0 left-0 z-50 md:z-auto
    w-72 h-full
    bg-mpx-surface border-r border-mpx-muted/20
    flex flex-col transition-transform duration-200 ease-in-out
    shrink-0
  "
>
  <!-- Sidebar header -->
  <div class="flex items-center justify-between px-3 py-3 border-b border-mpx-muted/20">
    <h3 class="text-sm font-semibold text-mpx-text tracking-wide">Conversations</h3>
    <button
      onclick={() => (sidebarOpen = false)}
      class="md:hidden text-mpx-muted hover:text-mpx-text cursor-pointer p-1"
      aria-label="Close sidebar"
    >
      <svg class="w-4 h-4" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path d="M6 18L18 6M6 6l12 12" />
      </svg>
    </button>
  </div>

  <!-- Search -->
  <div class="px-3 py-2">
    <input
      bind:value={searchText}
      placeholder="Search conversations…"
      class="w-full rounded-md bg-mpx-bg border border-mpx-muted/20 px-2.5 py-1.5
             text-xs text-mpx-text placeholder:text-mpx-muted/60
             outline-none focus:border-mpx-orange/50 transition-colors"
    />
  </div>

  <!-- Conversation list -->
  <div class="flex-1 overflow-y-auto min-h-0">
    {#if filtered.length === 0}
      <p class="text-center text-mpx-muted text-xs mt-8 px-3">
        {searchText ? "No matching conversations." : "No conversations yet."}
      </p>
    {:else}
      {#each filtered as convo (convo.id)}
        <div class="group">
          <button
            onclick={() => handleSelect(convo.id)}
            class="w-full text-left px-3 py-2.5 border-b border-mpx-muted/10
                   hover:bg-mpx-bg/40 transition-colors cursor-pointer
                   {convo.id === activeId ? 'bg-mpx-orange/10 border-l-2 border-l-mpx-orange' : ''}"
          >
            <div class="flex items-start justify-between gap-2">
              <div class="min-w-0 flex-1">
                <p class="text-sm text-mpx-text truncate leading-snug">{convo.title}</p>
                <p class="text-[10px] text-mpx-muted mt-0.5">
                  {formatTime(convo.updatedAt)}
                  {#if convo.messages?.length}
                    · {convo.messages.length} message{convo.messages.length === 1 ? "" : "s"}
                  {/if}
                </p>
              </div>
              <!-- svelte-ignore a11y_click_events_have_key_events -->
              <!-- svelte-ignore a11y_no_static_element_interactions -->
              <span
                onclick={(e) => handleDelete(convo.id, e)}
                class="shrink-0 p-0.5 rounded text-mpx-muted hover:text-red-400
                       hover:bg-red-400/10 transition-colors cursor-pointer opacity-0
                       group-hover:opacity-100 {convo.id === activeId ? 'opacity-100' : ''}"
                aria-label="Delete conversation"
              >
                <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
                  <path d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
                </svg>
              </span>
            </div>
          </button>
        </div>
      {/each}
    {/if}
  </div>

  <!-- Sidebar footer: export/import -->
  <div class="flex items-center gap-2 px-3 py-2.5 border-t border-mpx-muted/20">
    <button
      onclick={handleExport}
      class="flex-1 text-xs text-mpx-muted hover:text-mpx-text transition-colors
             px-2 py-1.5 rounded border border-mpx-muted/20 hover:border-mpx-orange/50
             cursor-pointer text-center"
    >
      Export
    </button>
    <button
      onclick={handleImport}
      class="flex-1 text-xs text-mpx-muted hover:text-mpx-text transition-colors
             px-2 py-1.5 rounded border border-mpx-muted/20 hover:border-mpx-orange/50
             cursor-pointer text-center"
    >
      Import
    </button>
  </div>
</aside>
