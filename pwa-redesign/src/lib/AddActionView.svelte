<script>
  import { colors } from "./colors.js";

  let { onSave, onBack } = $props();

  const YELLOW = colors.mpx.primary;

  // ── Available .wasm / .lua files (mock) ──
  let availableFiles = $state([
    { path: "/skills/walk.lua",       name: "walk.lua",       type: "lua" },
    { path: "/skills/sit.lua",        name: "sit.lua",        type: "lua" },
    { path: "/skills/dance.lua",      name: "dance.lua",      type: "lua" },
    { path: "/skills/follow.lua",     name: "follow.lua",     type: "lua" },
    { path: "/skills/lie.lua",        name: "lie.lua",        type: "lua" },
    { path: "/skills/walking/forward.wasm",  name: "forward.wasm",  type: "wasm" },
    { path: "/skills/walking/backward.wasm", name: "backward.wasm", type: "wasm" },
    { path: "/skills/walking/turn.wasm",     name: "turn.wasm",     type: "wasm" },
    { path: "/skills/tricks/spin.wasm",      name: "spin.wasm",     type: "wasm" },
    { path: "/skills/tricks/jump.wasm",      name: "jump.wasm",     type: "wasm" },
  ]);

  let selectedFile = $state(null);
  let actionName = $state("");
  let actionEmoji = $state("");

  const emojiSuggestions = [
    "🤖", "⚡", "🔥", "💨", "🌟", "🎯", "🎪", "🎭",
    "🦿", "🦾", "⚙️", "🔧", "🛠️", "🧠", "👀", "🦅",
    "🐉", "🦎", "🐢", "🐇", "🦊", "🐺", "🦁", "🐯",
    "🤿", "🏋️", "🤺", "🏃", "🧗", "🤹", "🎨", "🎵",
  ];

  let fileSearchQuery = $state("");

  let filteredFiles = $derived(
    fileSearchQuery
      ? availableFiles.filter(f =>
          f.name.toLowerCase().includes(fileSearchQuery.toLowerCase()) ||
          f.path.toLowerCase().includes(fileSearchQuery.toLowerCase())
        )
      : availableFiles
  );

  function selectFile(file) {
    selectedFile = file;
    // Auto-suggest a name from the file name
    if (!actionName) {
      actionName = file.name.replace(/\.(wasm|lua)$/, "");
      // Capitalise first letter
      actionName = actionName.charAt(0).toUpperCase() + actionName.slice(1);
    }
  }

  function handleSave() {
    if (!selectedFile || !actionName.trim()) return;

    const newAction = {
      id: "custom_" + Date.now(),
      label: actionName.trim(),
      emoji: actionEmoji || "⚙️",
      color: "#888",
      category: "custom",
      filePath: selectedFile.path,
      fileType: selectedFile.type,
    };

    onSave?.(newAction);
    onBack?.();
  }

  function isFormValid() {
    return selectedFile && actionName.trim().length > 0;
  }
</script>

<div class="add-action-page">
  <!-- Header -->
  <header class="top-bar" style="background: {YELLOW}">
    <button class="back-btn" onclick={onBack} aria-label="Back">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h1 class="top-bar-title">Add Action</h1>
    <div style="width:32px"></div>
  </header>

  <div class="form-scroll">
    <!-- ═══ Step 1: Pick a file ═══ -->
    <section class="form-section">
      <h2 class="section-title">1. Choose a file</h2>
      <p class="section-desc">Select a .wasm or .lua file to use for this action.</p>

      <!-- File search -->
      <div class="file-search-bar">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="11" cy="11" r="8"/>
          <line x1="21" y1="21" x2="16.65" y2="16.65"/>
        </svg>
        <input
          type="text"
          class="file-search-input"
          placeholder="Search files..."
          bind:value={fileSearchQuery}
        />
      </div>

      <div class="file-list">
        {#each filteredFiles as file}
          <button
            class="file-item"
            class:selected={selectedFile === file}
            onclick={() => selectFile(file)}
          >
            <span class="file-icon">{file.type === "wasm" ? "⚡" : "🌙"}</span>
            <div class="file-info">
              <span class="file-name">{file.name}</span>
              <span class="file-path">{file.path}</span>
            </div>
            {#if selectedFile === file}
              <span class="file-check">✓</span>
            {/if}
          </button>
        {/each}
      </div>
    </section>

    <!-- ═══ Step 2: Name ═══ -->
    <section class="form-section">
      <h2 class="section-title">2. Give it a name</h2>
      <input
        type="text"
        class="name-input"
        placeholder="e.g. My Cool Action"
        bind:value={actionName}
        maxlength="24"
      />
      <span class="char-count">{actionName.length}/24</span>
    </section>

    <!-- ═══ Step 3: Pick an emoji ═══ -->
    <section class="form-section">
      <h2 class="section-title">3. Pick an emoji</h2>
      <div class="emoji-grid">
        {#each emojiSuggestions as emoji}
          <button
            class="emoji-item"
            class:selected={actionEmoji === emoji}
            onclick={() => actionEmoji = emoji}
          >
            {emoji}
          </button>
        {/each}
      </div>
      {#if actionEmoji}
        <div class="emoji-preview">
          Selected: <span class="emoji-preview-icon">{actionEmoji}</span>
        </div>
      {/if}
    </section>

    <!-- ═══ Preview ═══ -->
    {#if selectedFile}
      <section class="form-section preview-section">
        <h2 class="section-title">Preview</h2>
        <div class="preview-card">
          <div class="preview-icon" style="background: {YELLOW}">
            <span class="preview-emoji">{actionEmoji || "⚙️"}</span>
          </div>
          <div class="preview-info">
            <span class="preview-name">{actionName || "Action Name"}</span>
            <span class="preview-file">{selectedFile.name}</span>
          </div>
        </div>
      </section>
    {/if}
  </div>

  <!-- ═══ Save Button ═══ -->
  <div class="footer-bar">
    <button
      class="save-btn"
      style="background: {YELLOW}"
      disabled={!isFormValid()}
      onclick={handleSave}
    >
      Save Action
    </button>
  </div>
</div>

<style>
  .add-action-page {
    position: absolute;
    inset: 0;
    background: #fff;
    display: flex;
    flex-direction: column;
    z-index: 50;
  }

  .top-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 14px;
    flex-shrink: 0;
  }

  .back-btn {
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: rgba(0,0,0,0.15);
    border-radius: 50%;
    cursor: pointer;
    color: #000;
    transition: background 0.15s;
  }
  .back-btn:hover { background: rgba(0,0,0,0.25); }

  .top-bar-title {
    font-size: 1rem;
    font-weight: 700;
    color: #000;
    margin: 0;
  }

  .form-scroll {
    flex: 1;
    overflow-y: auto;
    padding: 16px 14px;
  }
  .form-scroll::-webkit-scrollbar {
    width: 4px;
  }
  .form-scroll::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  /* ── Sections ── */
  .form-section {
    margin-bottom: 20px;
  }

  .section-title {
    font-size: 0.85rem;
    font-weight: 700;
    color: #000;
    margin: 0 0 4px;
  }

  .section-desc {
    font-size: 0.75rem;
    color: #999;
    margin: 0 0 10px;
  }

  /* ── File search ── */
  .file-search-bar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 12px;
    border-radius: 20px;
    background: #f5f5f5;
    border: 1.5px solid #e0e0e0;
    margin-bottom: 10px;
    color: #999;
  }
  .file-search-bar:focus-within {
    border-color: #FFE605;
    background: #fff;
  }

  .file-search-input {
    flex: 1;
    border: none;
    background: transparent;
    outline: none;
    font-size: 0.8rem;
    color: #000;
  }
  .file-search-input::placeholder {
    color: #bbb;
  }

  /* ── File list ── */
  .file-list {
    display: flex;
    flex-direction: column;
    gap: 6px;
    max-height: 200px;
    overflow-y: auto;
  }
  .file-list::-webkit-scrollbar {
    width: 3px;
  }
  .file-list::-webkit-scrollbar-thumb {
    background: #ddd;
    border-radius: 4px;
  }

  .file-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border-radius: 12px;
    border: 2px solid transparent;
    background: #fafafa;
    cursor: pointer;
    transition: all 0.15s;
    text-align: left;
    width: 100%;
  }
  .file-item:hover {
    background: #f0f0f0;
  }
  .file-item.selected {
    border-color: #FFE605;
    background: #fff;
  }

  .file-icon {
    font-size: 1.2rem;
    flex-shrink: 0;
  }

  .file-info {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }

  .file-name {
    font-size: 0.8rem;
    font-weight: 600;
    color: #000;
  }

  .file-path {
    font-size: 0.65rem;
    color: #999;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .file-check {
    margin-left: auto;
    color: #FFE605;
    font-weight: 700;
    font-size: 1rem;
    flex-shrink: 0;
  }

  /* ── Name input ── */
  .name-input {
    width: 100%;
    padding: 12px 14px;
    border: 2px solid #e0e0e0;
    border-radius: 12px;
    font-size: 0.9rem;
    outline: none;
    transition: border-color 0.15s;
    box-sizing: border-box;
    font-family: inherit;
  }
  .name-input:focus {
    border-color: #FFE605;
  }

  .char-count {
    display: block;
    text-align: right;
    font-size: 0.65rem;
    color: #bbb;
    margin-top: 4px;
  }

  /* ── Emoji picker ── */
  .emoji-grid {
    display: grid;
    grid-template-columns: repeat(8, 1fr);
    gap: 6px;
  }

  .emoji-item {
    width: 100%;
    aspect-ratio: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 2px solid transparent;
    border-radius: 10px;
    background: #f5f5f5;
    cursor: pointer;
    font-size: 1.2rem;
    transition: all 0.15s;
    padding: 0;
  }
  .emoji-item:hover {
    background: #eee;
  }
  .emoji-item.selected {
    border-color: #FFE605;
    background: #fff;
    transform: scale(1.1);
  }

  .emoji-preview {
    font-size: 0.8rem;
    color: #666;
    margin-top: 8px;
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .emoji-preview-icon {
    font-size: 1.5rem;
  }

  /* ── Preview ── */
  .preview-section {
    margin-bottom: 0;
  }

  .preview-card {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 14px;
    border-radius: 14px;
    background: #fafafa;
    border: 2px solid #eee;
  }

  .preview-icon {
    width: 52px;
    height: 52px;
    border-radius: 14px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.6rem;
    flex-shrink: 0;
  }

  .preview-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .preview-name {
    font-size: 0.95rem;
    font-weight: 700;
    color: #000;
  }

  .preview-file {
    font-size: 0.7rem;
    color: #999;
  }

  /* ── Footer ── */
  .footer-bar {
    padding: 12px 14px;
    border-top: 1px solid #eee;
    flex-shrink: 0;
  }

  .save-btn {
    width: 100%;
    padding: 14px;
    border: none;
    border-radius: 14px;
    font-size: 1rem;
    font-weight: 700;
    color: #000;
    cursor: pointer;
    transition: opacity 0.15s;
  }
  .save-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .save-btn:not(:disabled):hover {
    opacity: 0.9;
  }
</style>
