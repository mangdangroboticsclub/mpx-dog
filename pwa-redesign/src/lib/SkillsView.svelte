<script>
  /**
   * SkillsView — the robot's skills, as skills.
   *
   * Anything pushed with `mpx-cli deploy` used to surface only as a row in the
   * raw LittleFS file browser: an emoji, a filename, a byte count. You could
   * not tell a skill from a config blob, "Run" popped a browser alert that said
   * "started" whether the module worked or trapped on its first instruction,
   * and marketplace skills lived on a completely separate screen buried in
   * Settings.
   *
   * This screen is the skill list, with the file browser demoted to a segment
   * you can switch to when you actually want to poke at the filesystem.
   *
   * Three sources are merged into one list:
   *   - GET /v1/skills/list   — .wasm / .mpxe at the LittleFS root (the CLI)
   *   - GET /v1/lua/list      — .lua scripts under /lua
   *   - GET /v1/marketplace/robot/skills — skills bought from the marketplace
   *
   * The marketplace call needs the robot's uplink, so it is allowed to fail
   * quietly: no uplink simply means that section is not rendered. A skill you
   * uploaded yourself must never be hidden because the internet is down.
   */
  import { colors, skillTypeColor, skillTypeLabel } from "./colors.js";
  import FileViewer from "./FileViewer.svelte";
  import {
    listRobotSkills,
    toggleSkill,
    deploySkill,
    enqueueLua,
  } from "./marketplaceApi.js";

  let { onNavigate } = $props();

  const YELLOW = colors.mpx.primary;

  /**
   * Last-known data, kept outside the component so it survives navigating away
   * and back. Coming back to this screen used to blank the list and re-run
   * every request; now the previous answer is on screen in the same frame and
   * is quietly replaced when the fresh one lands.
   *
   * Deliberately module-level and NOT localStorage: it is a render cache for
   * this browsing session, not a record of anything. The robot owns what is
   * installed on it — that mistake was made once already and is not repeated
   * here.
   */
  const cache = { wasm: null, lua: null, fs: null, installed: null, mkt: null };
  const hydrated = cache.wasm !== null;

  // ── Which segment is showing ────────────────────────────────
  let segment = $state("skills");   // "skills" | "files"

  // ── Local (on-robot) skills ─────────────────────────────────
  let wasmSkills = $state(cache.wasm ?? []);   // [{ name, size }]
  let luaSkills = $state(cache.lua ?? []);     // [{ name }]
  let fsInfo = $state(cache.fs ?? { total: 0, used: 0 });
  let loading = $state(!hydrated);
  let loadError = $state("");

  // ── Marketplace skills ──────────────────────────────────────
  let mktSkills = $state(cache.mkt ?? []);
  let mktAvailable = $state(false);
  let mktLoading = $state(false);
  let actionInFlight = $state(null);

  // What the ROBOT records as installed. This used to be a localStorage map
  // built by regex-scraping Lua deploy scripts for file_write("...") calls,
  // which meant: install on your phone and your laptop offered to install
  // again; uninstall from a second device deleted nothing while reporting
  // success; and clearing site data orphaned every installed file with no way
  // to find it. The robot owns its filesystem, so the robot owns the record.
  let installed = $state(cache.installed ?? []);  // [{ skill_id, file, version, title }]

  // ── Run state ───────────────────────────────────────────────
  let running = $state(null);       // { name, elapsedMs } while a skill runs
  let lastRun = $state(null);       // { name, ok, message, durationMs }
  let runError = $state("");
  let statusTimer = null;

  // ── Confirmations (no window.confirm: it blocks the PWA shell) ──
  let confirmDelete = $state(null); // { kind, name, label }

  // ── Helpers ─────────────────────────────────────────────────
  function fmtSize(b) {
    if (b === undefined || b === null) return "";
    return b < 1024 ? b + " B" : (b / 1024).toFixed(1) + " KB";
  }

  /** walk_with_gains.wasm → "Walk With Gains" */
  function prettyName(filename) {
    return filename
      .replace(/\.(wasm|mpxe|lua)$/i, "")
      .replace(/[_-]+/g, " ")
      .replace(/\s+/g, " ")
      .trim()
      .replace(/\b\w/g, (c) => c.toUpperCase()) || filename;
  }

  function initialOf(name) {
    const p = prettyName(name);
    return (p.charAt(0) || "?").toUpperCase();
  }

  // ── Fetch ───────────────────────────────────────────────────
  /**
   * A request that gives up instead of hanging.
   *
   * The robot's httpd answers every request from a *single* task, so one slow
   * handler does not merely delay itself — it delays everything queued behind
   * it. A fetch with no deadline turns one wedged endpoint into a wedged
   * screen, which is exactly how this view came to sit on a spinner.
   */
  async function get(path, ms) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), ms);
    try {
      return await fetch(path, { signal: ctl.signal });
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * The critical path: the two lists this screen exists to show, and nothing
   * else. Storage figures, provenance badges and the marketplace all used to
   * be awaited before the first skill could be drawn — so the slowest of five
   * requests, one of which leaves the robot entirely, decided how long you
   * stared at "Loading…".
   */
  async function fetchSkillList() {
    loadError = "";
    try {
      const [sr, lr] = await Promise.all([
        get("/v1/skills/list", 8000),
        get("/v1/lua/list", 8000).catch(() => null),
      ]);

      if (!sr || !sr.ok) throw new Error("robot unreachable");
      wasmSkills = await sr.json();

      if (lr && lr.ok) {
        const d = await lr.json();
        // /v1/lua/list answers with a bare array of names.
        const names = Array.isArray(d) ? d : (d.files || d.f || []);
        luaSkills = names
          .map((n) => (typeof n === "string" ? { name: n } : n))
          // The deploy pipeline writes throwaway /lua/_deploy_N.lua files;
          // they are machinery, not something anyone wants to run by hand.
          .filter((s) => s.name && !s.name.startsWith("_deploy_"));
      } else {
        luaSkills = [];
      }
      cache.wasm = wasmSkills;
      cache.lua = luaSkills;
    } catch (e) {
      loadError = e.name === "AbortError"
        ? "The robot did not answer in time"
        : (e.message || "Could not reach the robot");
      wasmSkills = [];
      luaSkills = [];
    }
    loading = false;
  }

  /* Decoration. Costs a full-partition traversal on the robot when its cache
     is cold, so it must never gate the list. */
  async function fetchFsInfo() {
    try {
      const r = await get("/v1/fs/info", 8000);
      if (r.ok) { fsInfo = await r.json(); cache.fs = fsInfo; }
    } catch { /* the storage bar simply does not appear */ }
  }

  async function fetchMarketplace() {
    mktLoading = true;
    try {
      const list = await listRobotSkills();
      mktSkills = Array.isArray(list) ? list : [];
      mktAvailable = true;
      cache.mkt = mktSkills;
    } catch {
      // No uplink, or not signed in. Not an error worth showing here.
      mktSkills = [];
      mktAvailable = false;
    } finally {
      mktLoading = false;
    }
  }

  async function fetchInstalled() {
    try {
      const r = await get("/v1/skills/installed", 8000);
      if (r.ok) {
        const d = await r.json();
        installed = Array.isArray(d.skills) ? d.skills : [];
        cache.installed = installed;
      }
    } catch {
      // Older firmware has no such endpoint. An empty list simply means the
      // provenance badges do not show; the skill list itself is unaffected.
      installed = [];
    }
  }

  /**
   * Paint the list, then fill in the rest around it.
   *
   * The three background calls are deliberately not awaited: each updates its
   * own corner of the screen when it lands, and any of them can fail or time
   * out without the list noticing. They are also fired in the order the eye
   * reaches them.
   */
  async function refresh() {
    loading = !hydrated;    // a warm cache is already on screen; do not blank it
    await fetchSkillList();

    // Chained, not fired together. The robot has 16 LWIP sockets in total and
    // its web server holds at most five; a browser will happily open a fresh
    // connection per parallel request, and every one it closes then sits in
    // TIME_WAIT holding a slot. Three at once is three sockets for no gain —
    // the server answers them one at a time regardless, because it serves
    // every request from a single task.
    fetchInstalled()
      .then(fetchFsInfo)
      .then(fetchMarketplace);
  }

  $effect(() => {
    refresh();
    pollStatus();
    return () => { if (statusTimer) clearTimeout(statusTimer); };
  });

  // ── Run + status polling ────────────────────────────────────
  /**
   * POST /v1/skills/run returns the moment the task is spawned — the outcome
   * lands up to 60 s later. So the result comes from polling
   * GET /v1/skills/status, which reports the running skill and the last one's
   * exit code. Polling is fast while something runs and idles off afterwards,
   * so a parked screen is not hammering an ESP32 once a second forever.
   */
  async function pollStatus() {
    if (statusTimer) { clearTimeout(statusTimer); statusTimer = null; }

    try {
      const r = await fetch("/v1/skills/status");
      if (r.ok) {
        const d = await r.json();
        running = d.running
          ? { name: d.name || "", elapsedMs: d.elapsed_ms || 0 }
          : null;
        lastRun = d.last
          ? {
              name: d.last.name,
              ok: !!d.last.ok,
              message: d.last.message || (d.last.ok ? "ok" : "failed"),
              durationMs: d.last.duration_ms || 0,
            }
          : null;
      }
    } catch { /* robot busy or offline — try again on the next tick */ }

    statusTimer = setTimeout(pollStatus, running ? 500 : 4000);
  }

  async function runSkill(name) {
    runError = "";
    try {
      const r = await fetch("/v1/skills/run", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill: name }),
      });

      if (r.status === 409) {
        runError = "Another skill is already running — only one at a time.";
        return;
      }
      if (!r.ok) {
        runError = `Could not start ${name} (HTTP ${r.status})`;
        return;
      }

      // Show the running state immediately rather than waiting up to half a
      // second for the next poll to confirm what we already know.
      running = { name, elapsedMs: 0 };
      lastRun = null;
      pollStatus();
    } catch (e) {
      runError = e.message || "Could not reach the robot";
    }
  }

  async function runLua(name) {
    runError = "";
    try {
      const r = await fetch("/v1/lua/run", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: "/lua/" + name }),
      });
      const d = await r.json();
      lastRun = {
        name,
        ok: !!d.ok,
        message: d.ok ? (d.output || "ok") : (d.error || "failed"),
        durationMs: 0,
      };
    } catch (e) {
      runError = e.message || "Could not reach the robot";
    }
  }

  // ── Delete ──────────────────────────────────────────────────
  function askDelete(kind, name) {
    confirmDelete = { kind, name, label: prettyName(name) };
  }

  async function doDelete() {
    if (!confirmDelete) return;
    const { kind, name } = confirmDelete;
    confirmDelete = null;
    actionInFlight = "del:" + name;

    try {
      const path = kind === "lua" ? "/lua/" + name : "/" + name;
      const r = await fetch("/v1/fs/delete", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path }),
      });
      if (!r.ok) runError = `Delete failed (HTTP ${r.status})`;
      await fetchLocal();
    } catch (e) {
      runError = e.message || "Delete failed";
    }
    actionInFlight = null;
  }

  // ── Marketplace actions (carried over unchanged) ────────────
  function isWasmSkill(skill) {
    return (skill.skill_type || "").toLowerCase() === "wasm";
  }

  function isDeployed(skill) {
    return installed.some((e) => e.skill_id === skill.skill_id);
  }

  async function handleToggle(skillId, enabled) {
    actionInFlight = skillId;
    try {
      await toggleSkill(skillId, enabled);
      await fetchMarketplace();
    } catch (e) {
      runError = e.message || "Toggle failed";
    }
    actionInFlight = null;
  }

  async function handleDeploy(skill) {
    actionInFlight = "deploy:" + skill.skill_id;
    try {
      const data = await deploySkill(skill.skill_id);
      const allScripts = [];
      for (const s of data.skills || []) {
        for (const cmd of s.commands || []) {
          allScripts.push(cmd.script);
          await enqueueLua(cmd.script);
        }
      }
      // No client-side bookkeeping: ask the robot what it now has.
      await refresh();
    } catch (e) {
      runError = e.message || "Download failed";
    }
    actionInFlight = null;
  }

  async function handleRemove(skill) {
    actionInFlight = "delete:" + skill.skill_id;
    try {
      // One call deletes the file and clears the record together. Doing them
      // separately is how a refunded skill stayed runnable, and how a deleted
      // file left a phantom install behind.
      const r = await fetch("/v1/skills/uninstall", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill_id: skill.skill_id }),
      });
      if (!r.ok) runError = `Uninstall failed (HTTP ${r.status})`;
      await refresh();
    } catch (e) {
      runError = e.message || "Uninstall failed";
    }
    actionInFlight = null;
  }

  // ── Derived ─────────────────────────────────────────────────
  let localCount = $derived(wasmSkills.length + luaSkills.length);
  let usedPct = $derived(
    fsInfo.total ? Math.min(100, (fsInfo.used / fsInfo.total) * 100) : 0
  );
</script>

<div class="sv-root" style="--yellow: {YELLOW}">
  <!-- ═══ Header ═══ -->
  <header class="sv-header">
    <div class="sv-header-row">
      <h2 class="sv-title">Skills</h2>
      <button class="sv-icon-btn" onclick={refresh} aria-label="Refresh">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="23 4 23 10 17 10"/>
          <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
        </svg>
      </button>
    </div>

    <div class="sv-segmented" role="tablist">
      <button
        class="sv-seg" class:sv-seg-on={segment === "skills"}
        role="tab" aria-selected={segment === "skills"}
        onclick={() => (segment = "skills")}
      >
        Skills{#if localCount}<span class="sv-seg-count">{localCount}</span>{/if}
      </button>
      <button
        class="sv-seg" class:sv-seg-on={segment === "files"}
        role="tab" aria-selected={segment === "files"}
        onclick={() => (segment = "files")}
      >
        Files
      </button>
    </div>
  </header>

  {#if segment === "files"}
    <!-- The raw filesystem browser, unchanged — one level down instead of
         being the only way to see a skill. -->
    <div class="sv-files-host">
      <FileViewer {onNavigate} embedded={true} />
    </div>
  {:else}
    <div class="sv-body">
      <!-- ═══ Running / last-result banner ═══ -->
      {#if running}
        <div class="sv-banner sv-banner-run">
          <span class="sv-spinner" aria-hidden="true"></span>
          <div class="sv-banner-text">
            <strong>{prettyName(running.name)}</strong>
            <span>running… {(running.elapsedMs / 1000).toFixed(1)}s</span>
          </div>
        </div>
      {:else if lastRun}
        <div class="sv-banner" class:sv-banner-ok={lastRun.ok} class:sv-banner-bad={!lastRun.ok}>
          <span class="sv-banner-mark">{lastRun.ok ? "✓" : "!"}</span>
          <div class="sv-banner-text">
            <strong>{prettyName(lastRun.name)}</strong>
            <!-- The separator is written as an expression because Svelte
                 trims the literal whitespace that precedes a block. -->
            <span>
              {lastRun.message}{#if lastRun.durationMs}{" · " + (lastRun.durationMs / 1000).toFixed(1) + "s"}{/if}
            </span>
          </div>
          <button class="sv-banner-x" onclick={() => (lastRun = null)} aria-label="Dismiss">✕</button>
        </div>
      {/if}

      {#if runError}
        <div class="sv-banner sv-banner-bad">
          <span class="sv-banner-mark">!</span>
          <div class="sv-banner-text"><span>{runError}</span></div>
          <button class="sv-banner-x" onclick={() => (runError = "")} aria-label="Dismiss">✕</button>
        </div>
      {/if}

      <!-- ═══ Storage ═══ -->
      {#if fsInfo.total}
        <div class="sv-storage">
          <div class="sv-storage-row">
            <span>Storage</span>
            <span>{fmtSize(fsInfo.used)} / {fmtSize(fsInfo.total)}</span>
          </div>
          <div class="sv-storage-track">
            <div class="sv-storage-fill" class:sv-storage-hot={usedPct > 85} style="width:{usedPct}%"></div>
          </div>
        </div>
      {/if}

      <!-- ═══ On this robot ═══ -->
      <section class="sv-section">
        <h3 class="sv-section-title">On this robot</h3>

        {#if loading}
          <p class="sv-muted">Loading…</p>
        {:else if loadError}
          <div class="sv-empty">
            <p class="sv-empty-title">Robot unreachable</p>
            <p class="sv-empty-desc">{loadError}</p>
            <button class="sv-btn sv-btn-primary" onclick={refresh}>Retry</button>
          </div>
        {:else if localCount === 0}
          <div class="sv-empty">
            <p class="sv-empty-title">No skills uploaded yet</p>
            <p class="sv-empty-desc">
              Build one with the SDK and push it in a single command:
            </p>
            <code class="sv-code">mpx-cli deploy</code>
            <p class="sv-empty-desc sv-empty-or">or use the ＋ button to upload a .wasm by hand.</p>
          </div>
        {:else}
          <div class="sv-list">
            {#each wasmSkills as s (s.name)}
              <article class="sv-card" class:sv-card-live={running?.name === s.name}>
                <div class="sv-card-top">
                  <div class="sv-avatar" style="--badge: {skillTypeColor('wasm')}">{initialOf(s.name)}</div>
                  <div class="sv-card-id">
                    <div class="sv-card-title-row">
                      <span class="sv-card-title">{prettyName(s.name)}</span>
                      <span class="sv-chip" style="--badge: {skillTypeColor('wasm')}">
                        {skillTypeLabel("wasm")}
                      </span>
                    </div>
                    <span class="sv-card-meta">{s.name} · {fmtSize(s.size)}</span>
                  </div>
                </div>
                <div class="sv-card-actions">
                  <button
                    class="sv-btn sv-btn-primary"
                    disabled={!!running}
                    onclick={() => runSkill(s.name)}
                  >
                    {running?.name === s.name ? "Running…" : "Run"}
                  </button>
                  <button
                    class="sv-btn sv-btn-ghost"
                    disabled={actionInFlight === "del:" + s.name}
                    onclick={() => askDelete("wasm", s.name)}
                  >
                    Delete
                  </button>
                </div>
              </article>
            {/each}

            {#each luaSkills as s (s.name)}
              <article class="sv-card">
                <div class="sv-card-top">
                  <div class="sv-avatar" style="--badge: {skillTypeColor('awa')}">{initialOf(s.name)}</div>
                  <div class="sv-card-id">
                    <div class="sv-card-title-row">
                      <span class="sv-card-title">{prettyName(s.name)}</span>
                      <span class="sv-chip" style="--badge: {skillTypeColor('awa')}">
                        {skillTypeLabel("awa")}
                      </span>
                    </div>
                    <span class="sv-card-meta">{s.name}</span>
                  </div>
                </div>
                <div class="sv-card-actions">
                  <button class="sv-btn sv-btn-primary" onclick={() => runLua(s.name)}>Run</button>
                  <button
                    class="sv-btn sv-btn-ghost"
                    disabled={actionInFlight === "del:" + s.name}
                    onclick={() => askDelete("lua", s.name)}
                  >
                    Delete
                  </button>
                </div>
              </article>
            {/each}
          </div>
        {/if}
      </section>

      <!-- ═══ Marketplace ═══ -->
      <!-- This one leaves the robot, so it lands well after the list above.
           Saying so beats an empty gap that looks like "you own nothing". -->
      {#if mktLoading && !mktSkills.length}
        <section class="sv-section">
          <h3 class="sv-section-title">From the marketplace</h3>
          <p class="sv-mkt-wait">Checking your marketplace skills…</p>
        </section>
      {/if}
      {#if mktAvailable && mktSkills.length > 0}
        <section class="sv-section">
          <h3 class="sv-section-title">From the marketplace</h3>
          <div class="sv-list">
            {#each mktSkills as skill (skill.skill_id)}
              <article class="sv-card">
                <div class="sv-card-top">
                  <div class="sv-avatar" style="--badge: {skillTypeColor(skill.skill_type)}">
                    {(skill.title || "?").charAt(0).toUpperCase()}
                  </div>
                  <div class="sv-card-id">
                    <div class="sv-card-title-row">
                      <span class="sv-card-title">{skill.title || "Unknown skill"}</span>
                      <span class="sv-chip" style="--badge: {skillTypeColor(skill.skill_type)}">
                        {skillTypeLabel(skill.skill_type)}
                      </span>
                    </div>
                    <span class="sv-card-meta">v{skill.current_version || "1.0"}</span>
                  </div>
                </div>
                <div class="sv-card-actions">
                  {#if isWasmSkill(skill)}
                    {#if isDeployed(skill)}
                      <button
                        class="sv-btn sv-btn-ghost"
                        disabled={actionInFlight === "delete:" + skill.skill_id}
                        onclick={() => handleRemove(skill)}
                      >
                        {actionInFlight === "delete:" + skill.skill_id ? "…" : "Remove from robot"}
                      </button>
                    {:else}
                      <button
                        class="sv-btn sv-btn-primary"
                        disabled={actionInFlight === "deploy:" + skill.skill_id}
                        onclick={() => handleDeploy(skill)}
                      >
                        {actionInFlight === "deploy:" + skill.skill_id ? "…" : "Download to robot"}
                      </button>
                    {/if}
                  {:else}
                    <label class="sv-toggle" class:sv-toggle-busy={actionInFlight === skill.skill_id}>
                      <input
                        type="checkbox"
                        checked={skill.enabled}
                        disabled={actionInFlight === skill.skill_id}
                        onchange={() => handleToggle(skill.skill_id, !skill.enabled)}
                      />
                      <span class="sv-toggle-track"></span>
                      <span class="sv-toggle-thumb"></span>
                    </label>
                  {/if}
                </div>
              </article>
            {/each}
          </div>
        </section>
      {/if}

      <div class="sv-spacer"></div>
    </div>

    <!-- ═══ Upload FAB ═══ -->
    <button class="sv-fab" onclick={() => onNavigate && onNavigate("upload")} aria-label="Upload a skill">
      <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="12" y1="5" x2="12" y2="19"/>
        <line x1="5" y1="12" x2="19" y2="12"/>
      </svg>
    </button>
  {/if}

  <!-- ═══ Delete confirmation ═══ -->
  {#if confirmDelete}
    <div class="sv-modal-back">
      <div class="sv-modal">
        <h4 class="sv-modal-title">Delete {confirmDelete.label}?</h4>
        <p class="sv-modal-desc">
          <code>{confirmDelete.name}</code> is removed from the robot's storage.
          You can push it again with <code>mpx-cli deploy</code>.
        </p>
        <div class="sv-modal-actions">
          <button class="sv-btn sv-btn-ghost" onclick={() => (confirmDelete = null)}>Cancel</button>
          <button class="sv-btn sv-btn-danger" onclick={doDelete}>Delete</button>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .sv-root {
    flex: 1;
    min-height: 0;
    background: #f5f5f5;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    position: relative;
  }

  /* ── Header ─────────────────────────────────────── */
  .sv-header {
    flex-shrink: 0;
    background: var(--yellow);
    padding: 10px 14px 10px;
  }

  .sv-header-row {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 9px;
  }

  .sv-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: #000;
    margin: 0;
  }

  .sv-icon-btn {
    margin-left: auto;
    width: 30px;
    height: 30px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    border-radius: 50%;
    background: rgba(0, 0, 0, 0.14);
    color: #000;
    cursor: pointer;
  }
  .sv-icon-btn:active { background: rgba(0, 0, 0, 0.24); }

  /* ── Segmented control ──────────────────────────── */
  .sv-segmented {
    display: flex;
    gap: 3px;
    background: rgba(0, 0, 0, 0.14);
    border-radius: 999px;
    padding: 3px;
  }

  .sv-seg {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 5px;
    padding: 6px 10px;
    border: none;
    border-radius: 999px;
    background: transparent;
    color: rgba(0, 0, 0, 0.62);
    font-size: 0.78rem;
    font-weight: 700;
    cursor: pointer;
    transition: background 0.15s, color 0.15s;
  }

  .sv-seg-on {
    background: #000;
    color: var(--yellow);
  }

  .sv-seg-count {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    min-width: 17px;
    height: 17px;
    padding: 0 4px;
    border-radius: 999px;
    background: rgba(0, 0, 0, 0.16);
    color: inherit;
    font-size: 0.64rem;
    font-weight: 800;
  }
  .sv-seg-on .sv-seg-count {
    background: var(--yellow);
    color: #000;
  }

  /* ── Body ───────────────────────────────────────── */
  .sv-body {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    -webkit-overflow-scrolling: touch;
    padding: 12px 14px 0;
  }

  .sv-files-host {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  .sv-spacer { height: 88px; }

  /* ── Banners ────────────────────────────────────── */
  .sv-banner {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border-radius: 12px;
    background: #fff;
    border-left: 4px solid #949393;
    margin-bottom: 10px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
  }

  .sv-banner-run { border-left-color: var(--yellow); }
  .sv-banner-ok  { border-left-color: #6AAE6C; }
  .sv-banner-bad { border-left-color: #ED7676; }

  .sv-banner-text {
    display: flex;
    flex-direction: column;
    gap: 1px;
    min-width: 0;
    flex: 1;
  }
  .sv-banner-text strong {
    font-size: 0.84rem;
    font-weight: 800;
    color: #000;
  }
  .sv-banner-text span {
    font-size: 0.74rem;
    color: #6b6b6b;
    overflow-wrap: anywhere;
  }

  .sv-banner-mark {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 22px;
    flex-shrink: 0;
    border-radius: 50%;
    font-size: 0.78rem;
    font-weight: 900;
    color: #fff;
    background: #949393;
  }
  .sv-banner-ok  .sv-banner-mark { background: #6AAE6C; }
  .sv-banner-bad .sv-banner-mark { background: #ED7676; }

  .sv-banner-x {
    border: none;
    background: none;
    color: #9b9b9b;
    font-size: 0.8rem;
    cursor: pointer;
    padding: 4px;
    flex-shrink: 0;
  }

  .sv-spinner {
    width: 18px;
    height: 18px;
    flex-shrink: 0;
    border: 2.5px solid rgba(0, 0, 0, 0.14);
    border-top-color: #000;
    border-radius: 50%;
    animation: sv-spin 0.7s linear infinite;
  }
  @keyframes sv-spin { to { transform: rotate(360deg); } }

  /* ── Storage ────────────────────────────────────── */
  .sv-storage {
    background: #fff;
    border-radius: 12px;
    padding: 9px 12px 11px;
    margin-bottom: 14px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
  }

  .sv-storage-row {
    display: flex;
    justify-content: space-between;
    font-size: 0.72rem;
    font-weight: 700;
    color: #6b6b6b;
    margin-bottom: 6px;
  }

  .sv-storage-track {
    height: 5px;
    border-radius: 999px;
    background: #e4e4e4;
    overflow: hidden;
  }

  .sv-storage-fill {
    height: 100%;
    border-radius: 999px;
    background: #000;
  }
  .sv-storage-hot { background: #ED7676; }

  /* ── Sections ───────────────────────────────────── */
  .sv-section { margin-bottom: 18px; }

  .sv-section-title {
    font-size: 0.68rem;
    font-weight: 800;
    letter-spacing: 0.07em;
    text-transform: uppercase;
    color: #8b8b8b;
    margin: 0 0 8px 2px;
  }

  .sv-muted {
    font-size: 0.8rem;
    color: #8b8b8b;
    margin: 6px 2px;
  }

  .sv-list {
    display: flex;
    flex-direction: column;
    gap: 9px;
  }

  /* ── Skill card ─────────────────────────────────── */
  .sv-card {
    background: #fff;
    border-radius: 14px;
    padding: 12px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
    border: 1.5px solid transparent;
  }

  .sv-card-live {
    border-color: var(--yellow);
    box-shadow: 0 0 0 3px rgba(255, 230, 5, 0.25);
  }

  .sv-card-top {
    display: flex;
    align-items: flex-start;
    gap: 10px;
  }

  .sv-avatar {
    width: 38px;
    height: 38px;
    flex-shrink: 0;
    border-radius: 11px;
    background: var(--badge, #949393);
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1rem;
    font-weight: 800;
  }

  .sv-card-id {
    min-width: 0;
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  .sv-card-title-row {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }

  .sv-card-title {
    font-size: 0.9rem;
    font-weight: 800;
    color: #000;
    overflow-wrap: anywhere;
  }

  .sv-chip {
    flex-shrink: 0;
    padding: 2px 7px;
    border-radius: 999px;
    background: var(--badge, #949393);
    color: #fff;
    font-size: 0.58rem;
    font-weight: 800;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }

  .sv-card-meta {
    font-size: 0.7rem;
    color: #8b8b8b;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    overflow-wrap: anywhere;
  }

  /* Actions sit on their own row so a long skill name never squeezes the
     buttons to unreadable slivers on a narrow phone. */
  .sv-card-actions {
    display: flex;
    gap: 7px;
    margin-top: 11px;
  }

  .sv-btn {
    flex: 1;
    padding: 8px 12px;
    border: none;
    border-radius: 9px;
    font-size: 0.78rem;
    font-weight: 800;
    cursor: pointer;
    white-space: nowrap;
  }
  .sv-btn:disabled { opacity: 0.45; cursor: default; }

  .sv-btn-primary { background: #000; color: var(--yellow); }
  .sv-btn-ghost   { background: #ececec; color: #333; }
  .sv-btn-danger  { background: #ED7676; color: #fff; }

  /* Placeholder for the one section that waits on the network, not the robot. */
  .sv-mkt-wait {
    margin: 0;
    padding: 13px 15px;
    background: #fff;
    border-radius: 14px;
    font-size: 0.78rem;
    color: #7b7b7b;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
  }

  /* ── Empty state ────────────────────────────────── */
  .sv-empty {
    background: #fff;
    border-radius: 14px;
    padding: 22px 18px;
    text-align: center;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
  }

  .sv-empty-title {
    font-size: 0.92rem;
    font-weight: 800;
    color: #000;
    margin: 0 0 5px;
  }

  .sv-empty-desc {
    font-size: 0.78rem;
    color: #7b7b7b;
    margin: 0 0 10px;
    line-height: 1.45;
  }
  .sv-empty-or { margin: 10px 0 0; font-size: 0.72rem; }

  .sv-code {
    display: inline-block;
    padding: 7px 13px;
    border-radius: 8px;
    background: #000;
    color: var(--yellow);
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.78rem;
    font-weight: 700;
  }

  /* ── Marketplace toggle ─────────────────────────── */
  .sv-toggle {
    position: relative;
    width: 46px;
    height: 26px;
    margin-left: auto;
    flex-shrink: 0;
    cursor: pointer;
  }
  .sv-toggle-busy { opacity: 0.5; }
  .sv-toggle input {
    position: absolute;
    opacity: 0;
    width: 100%;
    height: 100%;
    margin: 0;
    cursor: pointer;
    z-index: 2;
  }
  .sv-toggle-track {
    position: absolute;
    inset: 0;
    border-radius: 999px;
    background: #d4d4d4;
    transition: background 0.18s;
  }
  .sv-toggle-thumb {
    position: absolute;
    top: 3px;
    left: 3px;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: #fff;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
    transition: transform 0.18s;
  }
  .sv-toggle input:checked ~ .sv-toggle-track { background: #000; }
  .sv-toggle input:checked ~ .sv-toggle-thumb { transform: translateX(20px); }

  /* ── FAB ────────────────────────────────────────── */
  .sv-fab {
    position: absolute;
    right: 16px;
    bottom: 18px;
    width: 52px;
    height: 52px;
    border: none;
    border-radius: 50%;
    background: var(--yellow);
    color: #000;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 3px 10px rgba(0, 0, 0, 0.25);
    cursor: pointer;
    z-index: 5;
  }
  .sv-fab:active { transform: scale(0.94); }

  /* ── Confirm modal ──────────────────────────────── */
  .sv-modal-back {
    position: absolute;
    inset: 0;
    background: rgba(0, 0, 0, 0.5);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 22px;
    z-index: 20;
  }

  .sv-modal {
    width: 100%;
    max-width: 330px;
    background: #fff;
    border-radius: 16px;
    padding: 18px;
  }

  .sv-modal-title {
    font-size: 0.98rem;
    font-weight: 800;
    color: #000;
    margin: 0 0 7px;
  }

  .sv-modal-desc {
    font-size: 0.78rem;
    color: #6b6b6b;
    line-height: 1.5;
    margin: 0 0 15px;
  }
  .sv-modal-desc code {
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.74rem;
    background: #f0f0f0;
    padding: 1px 5px;
    border-radius: 4px;
    overflow-wrap: anywhere;
  }

  .sv-modal-actions { display: flex; gap: 8px; }

  /* ── Wider screens: two cards per row ───────────── */
  @media (min-width: 720px) {
    .sv-body { padding: 16px 20px 0; }

    /* Left unconstrained this stretches to the full window width on a
       desktop browser, which reads as a banner rather than a control. */
    .sv-segmented { max-width: 360px; }

    .sv-list {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
      gap: 12px;
    }

    .sv-card { display: flex; flex-direction: column; }
    .sv-card-actions { margin-top: auto; padding-top: 11px; }
  }
</style>
