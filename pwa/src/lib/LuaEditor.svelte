<script lang="ts">
  import { onMount } from "svelte";
  import { basicSetup, EditorView } from "codemirror";
  import { EditorState } from "@codemirror/state";
  import { keymap } from "@codemirror/view";
  import { indentWithTab } from "@codemirror/commands";
  import { StreamLanguage } from "@codemirror/language";
  import { lua } from "@codemirror/legacy-modes/mode/lua";
  import { autocompletion } from "@codemirror/autocomplete";
  import { oneDark } from "@codemirror/theme-one-dark";
  import { robotCompletionSource } from "./lua-robot-api";

  let { navigate } = $props();

  // ── State ──────────────────────────────────────────────────
  let files: string[] = $state([]);
  let currentFile: string | null = $state(null);
  let dirty = $state(false);
  let output = $state("");
  let running = $state(false);
  let saving = $state(false);
  let loading = $state(true);
  let listError = $state("");
  let execError = $state("");

  // Mobile tabs
  let isMobile = $state(false);
  let mobileTab = $state<"files" | "editor" | "output">("editor");

  // Editor
  let editorElement: HTMLDivElement | undefined = $state(undefined);
  let editorView: EditorView | null = null;
  let editorReady = $state(false);
  let suppressingUpdate = false;

  // New file dialog
  let showNewFile = $state(false);
  let newFileName = $state("");

  // ── Mobile detection ───────────────────────────────────────
  $effect(() => {
    const mq = window.matchMedia("(max-width: 768px)");
    isMobile = mq.matches;
    const handler = (e: MediaQueryListEvent) => {
      isMobile = e.matches;
      if (!e.matches) mobileTab = "editor";
    };
    mq.addEventListener("change", handler);
    return () => mq.removeEventListener("change", handler);
  });

  // ── Initialise CodeMirror editor ───────────────────────────
  $effect(() => {
    if (!editorElement) return;

    const state = EditorState.create({
      doc: "-- Write your Lua script here\n\n",
      extensions: [
        basicSetup,
        StreamLanguage.define(lua),
        oneDark,
        keymap.of([indentWithTab]),
        autocompletion({ override: [robotCompletionSource] }),
        EditorView.updateListener.of((update) => {
          if (update.docChanged && !suppressingUpdate) {
            dirty = true;
          }
          suppressingUpdate = false;
        }),
      ],
    });

    editorView = new EditorView({ state, parent: editorElement });
    editorReady = true;

    return () => {
      editorView?.destroy();
      editorView = null;
      editorReady = false;
    };
  });

  // ── Helpers ────────────────────────────────────────────────
  function setEditorContent(content: string) {
    if (!editorView) return;
    suppressingUpdate = true;
    editorView.dispatch({
      changes: {
        from: 0,
        to: editorView.state.doc.length,
        insert: content,
      },
    });
  }

  function getEditorContent(): string {
    return editorView ? editorView.state.doc.toString() : "";
  }

  // ── File operations ────────────────────────────────────────
  async function listFiles() {
    loading = true;
    listError = "";
    try {
      const res = await fetch("/v1/lua/list");
      if (res.ok) {
        const data = await res.json();
        files = data.files || [];
      } else {
        listError = `List failed (${res.status})`;
      }
    } catch (e: any) {
      listError = `Cannot reach robot: ${e.message}`;
    }
    loading = false;
  }

  async function loadFile(name: string) {
    try {
      const res = await fetch("/v1/lua/read?name=" + encodeURIComponent(name));
      if (res.ok) {
        const data = await res.json();
        if (data.ok) {
          currentFile = name;
          setEditorContent(data.code || "");
          dirty = false;
          output = "";
          execError = "";
          if (isMobile) mobileTab = "editor";
          return;
        }
      }
      listError = `Failed to load ${name}`;
    } catch (e: any) {
      listError = `Error: ${e.message}`;
    }
  }

  async function saveFile() {
    if (!currentFile) {
      showNewFile = true;
      return;
    }
    await doSave(currentFile);
  }

  async function doSave(name: string) {
    saving = true;
    try {
      const code = getEditorContent();
      const res = await fetch("/v1/lua/save", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, code }),
      });
      if (res.ok) {
        dirty = false;
        currentFile = name;
        await listFiles();
      } else {
        const text = await res.text();
        execError = text || "Save failed";
      }
    } catch (e: any) {
      execError = `Save error: ${e.message}`;
    }
    saving = false;
  }

  function createNewFile() {
    const name = newFileName.trim();
    if (!name) return;
    if (!name.endsWith(".lua")) {
      execError = "Name must end with .lua";
      return;
    }
    showNewFile = false;
    newFileName = "";
    currentFile = name;
    setEditorContent("-- " + name + "\n\n");
    dirty = true;
    execError = "";
    doSave(name);
  }

  function closeNewFile() {
    showNewFile = false;
    newFileName = "";
  }

  function newScript() {
    currentFile = null;
    setEditorContent("-- New Lua script\n\n");
    dirty = false;
    output = "";
    execError = "";
    if (isMobile) mobileTab = "editor";
  }

  async function runScript() {
    const code = getEditorContent();
    if (!code.trim()) return;

    running = true;
    output = "";
    execError = "";

    try {
      if (currentFile) {
        // Save then run via path
        await fetch("/v1/lua/save", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name: currentFile, code }),
        });
        const res = await fetch("/v1/lua/run", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ path: "/lua/" + currentFile }),
        });
        const data = await res.json();
        if (data.ok) {
          output = data.output || "(no output)";
        } else {
          output = "❌ " + (data.error || "Execution failed");
          execError = "Execution failed";
        }
      } else {
        // Run inline
        const res = await fetch("/v1/lua/run", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ script: code }),
        });
        const data = await res.json();
        if (data.ok) {
          output = data.output || "(no output)";
        } else {
          output = "❌ " + (data.error || "Execution failed");
          execError = "Execution failed";
        }
      }
    } catch (e: any) {
      output = "❌ Error: " + e.message;
      execError = e.message;
    }

    running = false;
    dirty = false;
    if (isMobile) mobileTab = "output";
  }

  async function deleteFile(name: string) {
    if (!confirm("Delete " + name + "?")) return;
    try {
      const res = await fetch("/v1/lua/delete", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name }),
      });
      if (res.ok) {
        if (currentFile === name) {
          currentFile = null;
          setEditorContent("-- New Lua script\n\n");
          dirty = false;
        }
        await listFiles();
      } else {
        const text = await res.text();
        execError = text || "Delete failed";
      }
    } catch (e: any) {
      execError = `Delete error: ${e.message}`;
    }
  }

  // ── Init ───────────────────────────────────────────────────
  onMount(() => {
    listFiles();
  });
</script>

<div class="flex flex-col h-full">
  <!-- Header -->
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20 shrink-0">
    <button onclick={() => navigate("home")}
            class="text-lg hover:text-mpx-orange transition-colors cursor-pointer">‹</button>
    <h2 class="font-semibold">Lua Editor</h2>

    <!-- New / Save / Run buttons -->
    <div class="ml-auto flex items-center gap-1.5">
      <button onclick={newScript}
              title="New script"
              class="px-2 py-1 rounded text-xs bg-mpx-bg border border-mpx-muted/20 text-mpx-muted hover:text-mpx-text cursor-pointer">
        ✚ New
      </button>
      <button onclick={saveFile}
              disabled={saving || !dirty}
              title="Save to robot"
              class="px-2 py-1 rounded text-xs cursor-pointer
                     {dirty
                       ? 'bg-emerald-800/40 text-emerald-300 hover:bg-emerald-800/60 border border-emerald-700/50'
                       : 'bg-mpx-bg text-mpx-muted/40 border border-mpx-muted/10 cursor-not-allowed'}">
        {saving ? "Saving…" : "💾 Save"}
      </button>
      <button onclick={runScript}
              disabled={running}
              class="px-2 py-1 rounded text-xs cursor-pointer
                     {running
                       ? 'bg-indigo-800/40 text-indigo-300 animate-pulse border border-indigo-700/50'
                       : 'bg-indigo-800/60 text-indigo-200 hover:bg-indigo-800/80 border border-indigo-700/50'}">
        {running ? "Running…" : "▶ Run"}
      </button>
    </div>
  </header>

  <!-- New file dialog -->
  {#if showNewFile}
    <div class="px-4 py-3 bg-mpx-surface/80 border-b border-mpx-muted/10">
      <div class="flex items-center gap-2 max-w-sm">
        <input
          bind:value={newFileName}
          onkeydown={(e) => e.key === "Enter" && createNewFile()}
          placeholder="script.lua"
          class="flex-1 rounded bg-mpx-bg border border-mpx-muted/20 px-3 py-1.5 text-sm text-mpx-text outline-none focus:border-mpx-orange/50"
        />
        <button onclick={createNewFile}
                class="px-3 py-1.5 rounded text-xs bg-emerald-800/40 text-emerald-300 hover:bg-emerald-800/60 cursor-pointer border border-emerald-700/50">
          Create
        </button>
        <button onclick={closeNewFile}
                class="px-3 py-1.5 rounded text-xs bg-mpx-bg text-mpx-muted hover:text-mpx-text cursor-pointer border border-mpx-muted/20">
          Cancel
        </button>
      </div>
      <p class="text-xs text-mpx-muted mt-1.5">Enter a name ending with <strong>.lua</strong></p>
    </div>
  {/if}

  <!-- Status bar -->
  {#if currentFile || dirty || execError}
    <div class="flex items-center gap-3 px-4 py-1.5 bg-mpx-surface/30 border-b border-mpx-muted/5 text-xs text-mpx-muted shrink-0">
      {#if currentFile}
        <span class="font-mono truncate">📄 {currentFile}</span>
      {:else}
        <span class="italic">Unsaved script</span>
      {/if}
      {#if dirty}
        <span class="text-yellow-400">● unsaved</span>
      {/if}
      {#if execError}
        <span class="text-red-400 ml-auto truncate">⚠ {execError}</span>
      {/if}
    </div>
  {/if}

  <!-- Mobile tab bar -->
  {#if isMobile}
    <div class="flex border-b border-mpx-muted/10 shrink-0">
      <button onclick={() => mobileTab = "files"}
              class="flex-1 py-2 text-xs cursor-pointer
                     {mobileTab === 'files'
                       ? 'text-mpx-orange border-b-2 border-mpx-orange bg-mpx-orange/5'
                       : 'text-mpx-muted hover:text-mpx-text'}">
        📁 Files
      </button>
      <button onclick={() => mobileTab = "editor"}
              class="flex-1 py-2 text-xs cursor-pointer
                     {mobileTab === 'editor'
                       ? 'text-mpx-orange border-b-2 border-mpx-orange bg-mpx-orange/5'
                       : 'text-mpx-muted hover:text-mpx-text'}">
        ✏️ Editor
      </button>
      <button onclick={() => mobileTab = "output"}
              class="flex-1 py-2 text-xs cursor-pointer
                     {mobileTab === 'output'
                       ? 'text-mpx-orange border-b-2 border-mpx-orange bg-mpx-orange/5'
                       : 'text-mpx-muted hover:text-mpx-text'}">
        📟 Output
      </button>
    </div>
  {/if}

  <!-- Main content -->
  <div class="flex-1 flex overflow-hidden {isMobile ? 'flex-col' : ''}">
    <!-- File sidebar (desktop) or file panel (mobile) -->
    {#if !isMobile || mobileTab === "files"}
      <div class="flex flex-col {isMobile ? 'flex-1 overflow-y-auto' : 'w-48 border-r border-mpx-muted/10 shrink-0'}">
        <div class="px-3 py-2 text-xs text-mpx-muted font-semibold border-b border-mpx-muted/10 shrink-0 flex items-center justify-between">
          <span>Scripts</span>
          <button onclick={listFiles}
                  class="text-mpx-muted hover:text-mpx-text cursor-pointer">↻</button>
        </div>
        <div class="flex-1 overflow-y-auto">
          {#if loading}
            <p class="px-3 py-4 text-xs text-mpx-muted text-center">Loading…</p>
          {:else if listError}
            <p class="px-3 py-4 text-xs text-red-400 text-center">{listError}</p>
          {:else if files.length === 0}
            <p class="px-3 py-4 text-xs text-mpx-muted text-center">
              No .lua files yet.<br>
              Click <strong>✚ New</strong> to create one.
            </p>
          {:else}
            <div class="py-1">
              {#each files as f}
                <div class="flex items-center gap-1 px-2 py-1.5 text-xs hover:bg-mpx-surface/40 cursor-pointer group
                            {currentFile === f ? 'bg-mpx-orange/10 border-l-2 border-mpx-orange' : 'border-l-2 border-transparent'}"
                     onclick={() => loadFile(f)}
                     role="button"
                     tabindex="0"
                     onkeydown={(e) => e.key === "Enter" && loadFile(f)}>
                  <span class="shrink-0">🌙</span>
                  <span class="truncate flex-1 text-mpx-text">{f}</span>
                  {#if currentFile === f && dirty}
                    <span class="text-yellow-400 shrink-0">●</span>
                  {/if}
                  <button onclick={(e) => { e.stopPropagation(); deleteFile(f); }}
                          class="shrink-0 px-1 rounded text-mpx-muted hover:text-red-400 opacity-0 group-hover:opacity-100 transition-opacity cursor-pointer"
                          title="Delete">✕</button>
                </div>
              {/each}
            </div>
          {/if}
        </div>
      </div>
    {/if}

    <!-- Editor + Output panel -->
    {#if !isMobile || mobileTab === "editor" || mobileTab === "output"}
      <div class="flex-1 flex flex-col min-w-0">
        <!-- Editor -->
        {#if !isMobile || mobileTab === "editor"}
          <div class="flex-1 min-h-0 {isMobile ? '' : output ? '' : 'flex-1'}">
            {#if !editorReady}
              <div class="flex items-center justify-center h-full text-xs text-mpx-muted">
                Loading editor…
              </div>
            {/if}
            <div bind:this={editorElement} class="h-full text-sm"></div>
          </div>
        {/if}

        <!-- Output console -->
        {#if output || (!isMobile)}
          <div class="shrink-0 border-t border-mpx-muted/10 {isMobile && mobileTab !== 'output' ? 'hidden' : ''}
                      {isMobile ? 'flex-1 overflow-y-auto' : 'max-h-48'}">
            <div class="flex items-center justify-between px-4 py-1.5 bg-mpx-surface/50 border-b border-mpx-muted/5">
              <span class="text-xs text-mpx-muted font-semibold">📟 Output</span>
              {#if output}
                <button onclick={() => { output = ""; execError = ""; }}
                        class="text-xs text-mpx-muted hover:text-mpx-text cursor-pointer">Clear</button>
              {/if}
            </div>
            <div class="overflow-y-auto p-3 {isMobile ? 'min-h-[120px]' : 'max-h-36'}">
              {#if output}
                <pre class="text-xs text-green-400 font-mono whitespace-pre-wrap">{output}</pre>
              {:else}
                <p class="text-xs text-mpx-muted italic">Run a script to see output here.</p>
              {/if}
              {#if running}
                <div class="flex items-center gap-2 mt-2 text-xs text-indigo-400">
                  <span class="w-2 h-2 rounded-full bg-indigo-400 animate-pulse"></span>
                  Running…
                </div>
              {/if}
            </div>
          </div>
        {/if}
      </div>
    {/if}
  </div>
</div>
