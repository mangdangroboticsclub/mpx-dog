<script>
  let { navigate } = $props();

  let files = $state([]);
  let dirs = $state([]);
  let currentDir = $state("/");
  let fsInfo = $state({ total: 0, used: 0 });
  let loading = $state(true);
  let deleting = $state(null);
  let sudoMode = $state(false);
  let showAll = $state(false);
  let fileContent = $state(null);
  let columns = $state([]);
  let isMobile = $state(window.matchMedia("(max-width: 768px)").matches);

  $effect(() => {
    const mq = window.matchMedia("(max-width: 768px)");
    const handler = (e) => { isMobile = e.matches; };
    mq.addEventListener("change", handler);
    return () => mq.removeEventListener("change", handler);
  });

  function isWasm(n) { return n.endsWith(".wasm") || n.endsWith(".mpxe"); }
  function isLua(n)  { return n.endsWith(".lua"); }
  function isAllowed(n) { return isWasm(n) || isLua(n); }

  function icon(n) {
    if (isWasm(n)) return "⚡";
    if (isLua(n))  return "🌙";
    if (n.endsWith(".gz")) return "📦";
    if (n.endsWith(".json")) return "📋";
    if (n.endsWith(".md")) return "📝";
    return "📄";
  }

  function fmtSize(b) {
    return b < 1024 ? b + " B" : (b / 1024).toFixed(1) + " KB";
  }

  async function fetchDir(dir) {
    loading = true;
    fileContent = null;
    try {
      const [lr, ir] = await Promise.all([
        fetch("/v1/fs/list?path=" + encodeURIComponent(dir)),
        fetch("/v1/fs/info"),
      ]);
      if (lr.ok) { const d = await lr.json(); files = d.f || []; dirs = d.d || []; currentDir = dir; }
      if (ir.ok) fsInfo = await ir.json();
    } catch { files = []; dirs = []; }
    loading = false;
  }

  async function viewFile(p) {
    try {
      const r = await fetch("/v1/fs/read?path=" + encodeURIComponent(p));
      if (r.ok) { const d = await r.json(); if (d.ok) fileContent = { path: d.p, content: d.c }; }
    } catch { fileContent = null; }
  }

  async function delFile(n) {
    if (!confirm("Delete " + n + "?")) return;
    deleting = n;
    try {
      const rel = currentDir === "/" ? "" : currentDir;
      const r = await fetch("/v1/fs/delete", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: rel + "/" + n, sudo: sudoMode }),
      });
      if (!r.ok) alert("Delete failed: " + (await r.text()));
      else { await fetchDir(currentDir); if (!isMobile) await rebuild(); }
    } catch (e) { alert("Error: " + e.message); }
    deleting = null;
  }

  async function runWasm(n) {
    const p = (currentDir === "/" ? "" : currentDir) + "/" + n;
    try {
      const r = await fetch("/v1/skills/run", { method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill: p }) });
      if (r.ok) { const d = await r.json(); alert("⚡ " + n + "\n" + (d.output || "OK")); }
      else alert("❌ " + n + " (" + r.status + ")");
    } catch (e) { alert("❌ " + e.message); }
  }

  async function runLua(n) {
    const p = (currentDir === "/" ? "" : currentDir) + "/" + n;
    try {
      const r = await fetch("/v1/lua/run", { method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: p }) });
      const d = await r.json();
      if (d.ok) alert("🌙 " + n + "\n" + (d.output || "(no output)"));
      else alert("❌ " + n + ": " + (d.error || "failed"));
    } catch (e) { alert("❌ " + e.message); }
  }

  function goDir(d) { fileContent = null; fetchDir(currentDir === "/" ? "/" + d : currentDir + "/" + d); }
  function goUp() {
    fileContent = null;
    if (currentDir === "/") return;
    const p = currentDir.split("/").filter(Boolean); p.pop();
    fetchDir(p.length === 0 ? "/" : "/" + p.join("/"));
  }

  let selDirs = $state([]);
  async function rebuild() {
    const cols = []; let dir = "/";
    for (const s of selDirs) {
      const r = await fetch("/v1/fs/list?path=" + encodeURIComponent(dir));
      if (r.ok) { const d = await r.json(); cols.push({ dir, files: d.f || [], dirs: d.d || [] }); }
      dir = dir === "/" ? "/" + s : dir + "/" + s;
    }
    const r = await fetch("/v1/fs/list?path=" + encodeURIComponent(dir));
    if (r.ok) { const d = await r.json(); cols.push({ dir, files: d.f || [], dirs: d.d || [] }); }
    columns = cols; currentDir = dir;
    files = columns[columns.length - 1]?.files || [];
    dirs = columns[columns.length - 1]?.dirs || [];
  }
  function selCol(d, i) { selDirs = selDirs.slice(0, i); selDirs.push(d); fileContent = null; rebuild(); }
  function goCol(i) { selDirs = selDirs.slice(0, i); fileContent = null; rebuild(); }

  $effect(() => {
    fetchDir("/");
    // For desktop, also init column browser
    if (!isMobile) rebuild();
  });
</script>

<div class="flex flex-col h-full">
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20 shrink-0">
    <button onclick={() => navigate("home")} class="text-lg hover:text-mpx-orange cursor-pointer">‹</button>
    <h2 class="font-semibold">Files</h2>
    <button onclick={() => { sudoMode = !sudoMode; }}
            class="ml-auto text-xs px-2 py-1 rounded cursor-pointer border
                   {sudoMode ? 'bg-red-800/40 text-red-300 border-red-700/50' : 'bg-mpx-bg text-mpx-muted border-mpx-muted/20'}">
      {sudoMode ? '🔓' : '🔒'}</button>
    <button onclick={() => { showAll = !showAll; }}
            class="text-xs px-2 py-1 rounded cursor-pointer border
                   {showAll ? 'bg-mpx-orange/20 text-mpx-orange border-mpx-orange/40' : 'bg-mpx-bg text-mpx-muted border-mpx-muted/20'}">
      {showAll ? 'All' : 'Safe'}</button>
    <button onclick={() => isMobile ? fetchDir(currentDir) : rebuild()}
            class="text-xs text-mpx-muted hover:text-mpx-text cursor-pointer">↻</button>
  </header>

  <div class="px-4 py-2 bg-mpx-surface/30 border-b border-mpx-muted/5">
    <div class="flex justify-between text-xs text-mpx-muted mb-1">
      <span class="truncate">📁 {currentDir === "/" ? "/" : currentDir}</span>
      <span class="shrink-0 ml-2">{fsInfo.total ? ((fsInfo.used / fsInfo.total) * 100).toFixed(0) : 0}% ({(fsInfo.used / 1024).toFixed(0)}KB)</span>
    </div>
    <div class="h-1.5 rounded-full bg-mpx-bg overflow-hidden">
      <div class="h-full rounded-full bg-mpx-orange transition-all" style="width:{fsInfo.total ? ((fsInfo.used / fsInfo.total) * 100) : 0}%"></div>
    </div>
  </div>

  {#if isMobile}
    <div class="flex-1 overflow-y-auto">
      {#if loading}
        <p class="text-center text-mpx-muted text-sm mt-8">Loading…</p>
      {:else}
        <div class="space-y-0.5 px-2 py-2">
          {#if currentDir !== "/"}
            <button onclick={goUp} class="w-full flex items-center gap-3 rounded-lg px-3 py-2.5 hover:bg-mpx-surface/50 cursor-pointer text-left">
              <span class="text-base shrink-0">📂</span><span class="text-sm text-mpx-muted font-medium">..</span>
            </button>
          {/if}
          {#each dirs as d}
            <button onclick={() => goDir(d)} class="w-full flex items-center gap-3 rounded-lg px-3 py-2.5 hover:bg-mpx-surface/50 cursor-pointer text-left">
              <span class="text-base shrink-0">📁</span><span class="text-sm text-mpx-text font-medium">{d}</span>
            </button>
          {/each}
          {#each files as f}
            {@const ok = isAllowed(f.n)}
            {#if showAll || ok}
              <div class="flex items-center justify-between rounded-lg px-3 py-2.5 hover:bg-mpx-surface/30 {!ok && !sudoMode ? 'opacity-50' : ''}">
                <div class="flex items-center gap-3 min-w-0 flex-1">
                  <span class="text-base shrink-0">{icon(f.n)}</span>
                  <div class="min-w-0 flex-1">
                    <p class="text-sm text-mpx-text truncate">{f.n}{#if f.r}<span class="text-mpx-muted/40 text-xs ml-1">🔒</span>{/if}</p>
                    <p class="text-xs text-mpx-muted">{fmtSize(f.s)}</p>
                  </div>
                </div>
                <div class="flex gap-1 shrink-0 ml-2">
                  {#if isLua(f.n)}
                    <button onclick={() => viewFile((currentDir === "/" ? "" : currentDir) + "/" + f.n)}
                            class="px-2.5 py-1 rounded text-xs bg-mpx-bg border border-mpx-muted/20 text-mpx-muted hover:text-mpx-text cursor-pointer" title="View">📖</button>
                    <button onclick={() => runLua(f.n)}
                            class="px-2.5 py-1 rounded text-xs bg-indigo-800/40 text-indigo-300 hover:bg-indigo-800/60 cursor-pointer" title="Run">▶</button>
                  {:else if isWasm(f.n)}
                    <button onclick={() => runWasm(f.n)}
                            class="px-2.5 py-1 rounded text-xs bg-mpx-orange/20 text-mpx-orange hover:bg-mpx-orange/40 cursor-pointer" title="Run">⚡</button>
                  {/if}
                  {#if !f.r && (sudoMode || ok)}
                    <button onclick={() => delFile(f.n)} disabled={deleting === f.n}
                            class="px-2.5 py-1 rounded text-xs bg-red-900/40 text-red-400 hover:bg-red-900/60 cursor-pointer {deleting === f.n ? 'opacity-50' : ''}" title="Delete">🗑</button>
                  {/if}
                </div>
              </div>
            {/if}
          {/each}
        </div>
      {/if}
      {#if fileContent}
        <div class="border-t border-mpx-muted/20 bg-black/40">
          <div class="flex items-center justify-between px-4 py-2">
            <span class="text-xs text-mpx-muted font-mono truncate">{fileContent.path}</span>
            <button onclick={() => { fileContent = null; }} class="text-xs text-mpx-muted hover:text-mpx-text cursor-pointer">✕</button>
          </div>
          <pre class="px-4 pb-3 text-xs text-green-400 font-mono whitespace-pre-wrap overflow-x-auto max-h-64">{fileContent.content || "(empty)"}</pre>
        </div>
      {/if}
    </div>
  {:else}
    {#if columns.length === 0}
      <p class="text-center text-mpx-muted text-sm w-full mt-8">Loading directory…</p>
    {:else}
    <div class="flex-1 flex overflow-hidden">
      {#each columns as col, ci}
        <div class="flex flex-col border-r border-mpx-muted/10 min-w-48 max-w-64 w-1/4 overflow-hidden {ci === columns.length - 1 ? 'flex-1 max-w-none' : ''}">
          <div class="px-3 py-1.5 bg-mpx-surface/50 border-b border-mpx-muted/10 shrink-0">
            <button onclick={() => goCol(ci)} class="text-xs text-mpx-muted hover:text-mpx-text truncate w-full text-left cursor-pointer {ci === columns.length - 1 ? 'font-semibold text-mpx-text' : ''}">
              {ci === 0 ? '/' : col.dir.split('/').pop()}
            </button>
          </div>
          <div class="flex-1 overflow-y-auto">
            {#if ci > 0}
              <button onclick={() => goCol(ci - 1)} class="w-full flex items-center gap-2 px-3 py-1.5 text-sm text-mpx-muted hover:bg-mpx-surface/40 cursor-pointer text-left">
                <span class="text-xs">📂</span><span class="text-xs">..</span>
              </button>
            {/if}
            {#each col.dirs as d}
              <button onclick={() => selCol(d, ci)} class="w-full flex items-center gap-2 px-3 py-1.5 text-sm text-mpx-text hover:bg-mpx-surface/40 cursor-pointer text-left {ci === columns.length - 1 ? '' : 'opacity-60'}">
                <span class="text-xs shrink-0">📁</span><span class="truncate">{d}</span>
              </button>
            {/each}
            {#each col.files as f}
              {@const ok = isAllowed(f.n)}
              {#if showAll || ok}
                <div class="flex items-center gap-2 px-3 py-1.5 text-sm {!ok && !sudoMode ? 'opacity-40' : ''}">
                  <span class="text-xs shrink-0">{icon(f.n)}</span>
                  <span class="truncate flex-1 text-mpx-text" title={f.n}>{f.n}{#if f.r}<span class="text-mpx-muted/40 text-xs">🔒</span>{/if}</span>
                  {#if isLua(f.n)}
                    <button onclick={() => viewFile(col.dir + "/" + f.n)} class="text-xs px-1.5 py-0.5 rounded text-mpx-muted hover:text-mpx-text hover:bg-mpx-surface/50 cursor-pointer" title="View">📖</button>
                    <button onclick={() => runLua(f.n)} class="text-xs px-1.5 py-0.5 rounded text-indigo-400 hover:bg-indigo-900/30 cursor-pointer" title="Run">▶</button>
                  {:else if isWasm(f.n)}
                    <button onclick={() => runWasm(f.n)} class="text-xs px-1.5 py-0.5 rounded text-mpx-orange hover:bg-mpx-orange/20 cursor-pointer" title="Run">⚡</button>
                  {/if}
                  {#if !f.r && (sudoMode || ok)}
                    <button onclick={() => delFile(f.n)} disabled={deleting === f.n} class="text-xs px-1.5 py-0.5 rounded text-red-400 hover:bg-red-900/30 cursor-pointer {deleting === f.n ? 'opacity-50' : ''}" title="Delete">✕</button>
                  {/if}
                </div>
              {/if}
            {/each}
          </div>
        </div>
      {/each}
      {#if fileContent}
        <div class="flex flex-col border-l border-mpx-muted/10 min-w-64 max-w-md overflow-hidden">
          <div class="flex items-center justify-between px-3 py-1.5 bg-mpx-surface/50 border-b border-mpx-muted/10 shrink-0">
            <span class="text-xs text-mpx-muted font-mono truncate">{fileContent.path}</span>
            <button onclick={() => { fileContent = null; }} class="text-xs text-mpx-muted hover:text-mpx-text cursor-pointer">✕</button>
          </div>
          <pre class="flex-1 overflow-y-auto px-3 py-2 text-xs text-green-400 font-mono whitespace-pre-wrap">{fileContent.content || "(empty)"}</pre>
        </div>
      {/if}
    </div>
    {/if}
  {/if}
</div>