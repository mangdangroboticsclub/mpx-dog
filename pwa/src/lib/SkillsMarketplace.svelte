<script>
  import {
    listSkills,
    getSkillManifest,
    listRobotSkills,
    assignSkill,
    toggleSkill,
    removeSkill,
    deploySkill,
    enqueueLua,
  } from "./marketplaceApi.js";

  let { navigate } = $props();

  // ── Tab state ─────────────────────────────────────────────
  let activeTab = $state("browse");  // "browse" | "toggles"

  // ── Browse tab state ──────────────────────────────────────
  let marketplaceSkills = $state([]);
  let browseLoading = $state(true);
  let browseError = $state("");

  // ── Toggles tab state ─────────────────────────────────────
  let robotSkills = $state([]);
  let togglesLoading = $state(true);
  let togglesError = $state("");

  // ── Detail view state ─────────────────────────────────────
  let detailSkill = $state(null);
  let detailManifest = $state(null);
  let detailLoading = $state(false);

  // ── Action state (track in-flight operations) ─────────────
  let actionInFlight = $state(null); // skill_id of the skill being acted upon

  // ── Build a lookup: assigned skill_id → robot skill object ──
  let robotSkillMap = $derived(() => {
    const map = {};
    for (const s of robotSkills) {
      map[s.skill_id] = s;
    }
    return map;
  });

  // ── Fetch marketplace skills ──────────────────────────────
  async function fetchMarketplace() {
    browseLoading = true;
    browseError = "";
    try {
      marketplaceSkills = await listSkills();
    } catch (e) {
      browseError = e.message || "Failed to load marketplace";
      marketplaceSkills = [];
    }
    browseLoading = false;
  }

  // ── Fetch robot's assigned skills ─────────────────────────
  async function fetchRobotSkills() {
    togglesLoading = true;
    togglesError = "";
    try {
      robotSkills = await listRobotSkills();
    } catch (e) {
      togglesError = e.message || "Failed to load assigned skills";
      robotSkills = [];
    }
    togglesLoading = false;
  }

  // ── Fetch both in parallel ────────────────────────────────
  async function refreshAll() {
    await Promise.all([fetchMarketplace(), fetchRobotSkills()]);
  }

  // ── Subscribe to a skill ──────────────────────────────────
  async function handleSubscribe(skillId) {
    actionInFlight = skillId;
    try {
      await assignSkill(skillId);
      // Only refresh robot skills — marketplace listing hasn't changed
      await fetchRobotSkills();
    } catch (e) {
      console.error("Subscribe failed:", e);
    }
    actionInFlight = null;
  }

  // ── Refund (unsubscribe) a skill ──────────────────────────
  async function handleRefund(skillId) {
    actionInFlight = skillId;
    try {
      await removeSkill(skillId);
      // Only refresh robot skills — marketplace listing hasn't changed
      await fetchRobotSkills();
    } catch (e) {
      console.error("Refund failed:", e);
    }
    actionInFlight = null;
  }

  // ── Toggle a skill on/off ─────────────────────────────────
  async function handleToggle(skillId, enabled) {
    actionInFlight = skillId;
    try {
      await toggleSkill(skillId, enabled);
      await fetchRobotSkills();
    } catch (e) {
      console.error("Toggle failed:", e);
    }
    actionInFlight = null;
  }

  // ── Download (deploy) a WASM skill ────────────────────────
  async function handleDeploy(skillId) {
    actionInFlight = "deploy:" + skillId;
    try {
      const data = await deploySkill(skillId);
      if (data.skills && data.skills.length > 0) {
        for (const skill of data.skills) {
          for (const cmd of skill.commands || []) {
            await enqueueLua(cmd.script);
          }
        }
      }
    } catch (e) {
      console.error("Deploy failed:", e);
    }
    actionInFlight = null;
  }

  function isWasmSkill(skill) {
    return skill.skill_type?.toLowerCase() === "wasm";
  }

  // ── Open skill detail ─────────────────────────────────────
  async function openDetail(skill) {
    detailSkill = skill;
    detailManifest = null;
    detailLoading = true;
    try {
      detailManifest = await getSkillManifest(skill.id);
    } catch {
      detailManifest = null;
    }
    detailLoading = false;
  }

  function closeDetail() {
    detailSkill = null;
    detailManifest = null;
  }

  // ── Check if a skill is assigned ──────────────────────────
  function isAssigned(skillId) {
    return robotSkills.some((s) => s.skill_id === skillId);
  }

  /**
   * Extract author from available data.
   * Priority: manifest.author → parse from skill ID (before `~`) → "Unknown"
   */
  function extractAuthor(skill) {
    if (detailManifest?.author) return detailManifest.author;
    if (skill?.id) {
      const parts = skill.id.split("~");
      if (parts.length >= 2 && parts[0]) {
        return parts[0]
          .replace(/_/g, " ")
          .replace(/\b\w/g, (c) => c.toUpperCase());
      }
    }
    return "Unknown";
  }

  function getAssignedSkill(skillId) {
    return robotSkills.find((s) => s.skill_id === skillId);
  }

  // ── Init ──────────────────────────────────────────────────
  $effect(() => {
    refreshAll();
  });
</script>

<div class="flex flex-col h-full">
  <!-- Header -->
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20 shrink-0">
    <button
      onclick={() => (detailSkill ? closeDetail() : navigate("home"))}
      class="text-lg hover:text-mpx-orange transition-colors cursor-pointer"
    >‹</button>
    <h2 class="font-semibold">{detailSkill ? detailSkill.title : "Marketplace"}</h2>
    {#if !detailSkill}
      <button
        onclick={refreshAll}
        class="ml-auto text-xs text-mpx-muted hover:text-mpx-text transition-colors cursor-pointer"
      >↻ Refresh</button>
    {/if}
  </header>

  <!-- Detail view -->
  {#if detailSkill}
    <div class="flex-1 overflow-y-auto px-4 py-3 space-y-4">
      {#if detailLoading}
        <p class="text-center text-mpx-muted text-sm mt-8">Loading details…</p>
      {:else if detailManifest}
        <!-- Skill icon + name -->
        <div class="flex items-center gap-4">
          {#if detailManifest.icon_url}
            <img
              src={detailManifest.icon_url}
              alt={detailManifest.name}
              class="w-16 h-16 rounded-xl bg-mpx-surface object-cover"
            />
          {:else}
            <div class="w-16 h-16 rounded-xl bg-mpx-orange/20 flex items-center justify-center text-2xl">⚡</div>
          {/if}
          <div>
            <h3 class="text-lg font-bold text-mpx-text">{detailManifest.name}</h3>
            <p class="text-xs text-mpx-muted">by {extractAuthor(detailSkill)} · v{detailManifest.version}</p>
          </div>
        </div>

        <!-- Description -->
        {#if detailManifest.description}
          <p class="text-sm text-mpx-text/80 leading-relaxed">{detailManifest.description}</p>
        {/if}

        <!-- Capabilities -->
        {#if detailManifest.capabilities?.length}
          <div>
            <h4 class="text-xs font-semibold text-mpx-muted uppercase tracking-wider mb-2">Capabilities</h4>
            <div class="flex flex-wrap gap-2">
              {#each detailManifest.capabilities as cap}
                <span class="rounded-full bg-mpx-orange/10 text-mpx-orange text-xs px-3 py-1">{cap}</span>
              {/each}
            </div>
          </div>
        {/if}

        <!-- Tags -->
        {#if detailManifest.tags?.length}
          <div>
            <h4 class="text-xs font-semibold text-mpx-muted uppercase tracking-wider mb-2">Tags</h4>
            <div class="flex flex-wrap gap-2">
              {#each detailManifest.tags as tag}
                <span class="rounded-full bg-mpx-surface border border-mpx-muted/20 text-mpx-muted text-xs px-3 py-1">{tag}</span>
              {/each}
            </div>
          </div>
        {/if}

        <!-- Action buttons -->
        <div class="flex gap-3 pt-2">
          {#if isAssigned(detailSkill.id)}
            <button
              onclick={() => handleRefund(detailSkill.id)}
              disabled={actionInFlight === detailSkill.id}
              class="flex-1 rounded-lg border border-red-500/50 text-red-400 px-4 py-2 text-sm
                     hover:bg-red-500/10 transition-colors cursor-pointer
                     {actionInFlight === detailSkill.id ? 'opacity-50' : ''}"
            >
              {actionInFlight === detailSkill.id ? "Refunding…" : "Refund"}
            </button>
          {:else}
            <button
              onclick={() => handleSubscribe(detailSkill.id)}
              disabled={actionInFlight === detailSkill.id}
              class="flex-1 rounded-lg bg-mpx-orange px-4 py-2 text-sm text-white
                     hover:bg-mpx-orange-light transition-colors cursor-pointer
                     {actionInFlight === detailSkill.id ? 'opacity-50' : ''}"
            >
              {actionInFlight === detailSkill.id ? "Subscribing…" : "Subscribe"}
            </button>
          {/if}
        </div>

        <!-- README -->
        {#if detailManifest.readme}
          <div class="mt-2">
            <h4 class="text-xs font-semibold text-mpx-muted uppercase tracking-wider mb-2">README</h4>
            <div class="rounded-lg bg-mpx-surface/50 border border-mpx-muted/10 px-4 py-3">
              <p class="text-xs text-mpx-text/70 whitespace-pre-wrap leading-relaxed">{detailManifest.readme}</p>
            </div>
          </div>
        {/if}
      {:else}
        <p class="text-center text-mpx-muted text-sm mt-8">Could not load skill details.</p>
      {/if}
    </div>

  {:else}
    <!-- Tabs -->
    <div class="flex border-b border-mpx-muted/20 shrink-0">
      <button
        onclick={() => (activeTab = "browse")}
        class="flex-1 py-2.5 text-sm font-medium text-center transition-colors cursor-pointer
               {activeTab === 'browse'
                 ? 'text-mpx-orange border-b-2 border-mpx-orange'
                 : 'text-mpx-muted hover:text-mpx-text'}"
      >Browse</button>
      <button
        onclick={() => (activeTab = "toggles")}
        class="flex-1 py-2.5 text-sm font-medium text-center transition-colors cursor-pointer
               {activeTab === 'toggles'
                 ? 'text-mpx-orange border-b-2 border-mpx-orange'
                 : 'text-mpx-muted hover:text-mpx-text'}"
      >My Skills</button>
    </div>

    <!-- Tab content -->
    <div class="flex-1 overflow-y-auto min-h-0">
      {#if activeTab === "browse"}
        <!-- Browse / Marketplace tab -->
        <div class="px-4 py-3 space-y-3">
          {#if browseLoading}
            <p class="text-center text-mpx-muted text-sm mt-8">Loading marketplace…</p>
          {:else if browseError}
            <div class="rounded-lg bg-red-900/30 border border-red-700/50 px-4 py-3 text-xs text-red-300 text-center">
              {browseError}
              <button onclick={fetchMarketplace}
                      class="block mx-auto mt-2 text-mpx-orange hover:underline cursor-pointer">Retry</button>
            </div>
          {:else if marketplaceSkills.length === 0}
            <p class="text-center text-mpx-muted text-sm mt-8">No skills available in the marketplace.</p>
          {:else}
            {#each marketplaceSkills as skill (skill.id)}
              <div class="rounded-lg bg-mpx-surface border border-mpx-muted/10 px-4 py-3
                          hover:border-mpx-muted/30 transition-colors">
                <button
                  onclick={() => openDetail(skill)}
                  class="w-full text-left cursor-pointer"
                >
                  <div class="flex items-center justify-between gap-3">
                    <div class="min-w-0 flex-1">
                      <p class="text-sm font-medium text-mpx-text truncate">{skill.title}</p>
                      <p class="text-xs text-mpx-muted mt-0.5">{skill.skill_type}</p>
                    </div>
                    <span class="text-xs text-mpx-muted shrink-0">v{skill.current_version}</span>
                  </div>
                </button>

                <!-- Action button -->
                <div class="mt-2 flex justify-end">
                  {#if isAssigned(skill.id)}
                    <button
                      onclick={() => handleRefund(skill.id)}
                      disabled={actionInFlight === skill.id}
                      class="rounded-lg border border-red-500/50 text-red-400 px-3 py-1 text-xs
                             hover:bg-red-500/10 transition-colors cursor-pointer
                             {actionInFlight === skill.id ? 'opacity-50' : ''}"
                    >
                      {actionInFlight === skill.id ? "…" : "Refund"}
                    </button>
                  {:else}
                    <button
                      onclick={() => handleSubscribe(skill.id)}
                      disabled={actionInFlight === skill.id}
                      class="rounded-lg bg-mpx-orange px-3 py-1 text-xs text-white
                             hover:bg-mpx-orange-light transition-colors cursor-pointer
                             {actionInFlight === skill.id ? 'opacity-50 animate-pulse' : ''}"
                    >
                      {actionInFlight === skill.id ? "…" : "Subscribe"}
                    </button>
                  {/if}
                </div>
              </div>
            {/each}
          {/if}
        </div>

      {:else}
        <!-- My Skills / Toggles tab -->
        <div class="px-4 py-3 space-y-2">
          {#if togglesLoading}
            <p class="text-center text-mpx-muted text-sm mt-8">Loading assigned skills…</p>
          {:else if togglesError}
            <div class="rounded-lg bg-red-900/30 border border-red-700/50 px-4 py-3 text-xs text-red-300 text-center">
              {togglesError}
              <button onclick={fetchRobotSkills}
                      class="block mx-auto mt-2 text-mpx-orange hover:underline cursor-pointer">Retry</button>
            </div>
          {:else if robotSkills.length === 0}
            <p class="text-center text-mpx-muted text-sm mt-8">No skills assigned yet.</p>
            <p class="text-center text-xs text-mpx-muted mt-1">
              Browse the marketplace and subscribe to a skill.
            </p>
          {:else}
            {#each robotSkills as skill (skill.skill_id)}
              <div class="rounded-lg bg-mpx-surface border border-mpx-muted/10 px-4 py-3">
                <div class="flex items-center justify-between gap-2">
                  <div class="min-w-0 flex-1">
                    <p class="text-sm font-medium text-mpx-text">{skill.title}</p>
                    <p class="text-xs text-mpx-muted mt-0.5">{skill.skill_type}</p>
                  </div>
                  <div class="flex items-center gap-2 shrink-0">
                    {#if isWasmSkill(skill)}
                      <button
                        onclick={() => handleDeploy(skill.skill_id)}
                        disabled={actionInFlight === "deploy:" + skill.skill_id}
                        class="rounded-lg bg-mpx-orange px-3 py-1 text-xs text-white
                               hover:bg-mpx-orange-light transition-colors cursor-pointer
                               {actionInFlight === 'deploy:' + skill.skill_id ? 'opacity-50' : ''}"
                      >
                        {actionInFlight === "deploy:" + skill.skill_id ? "…" : "Download"}
                      </button>
                    {/if}
                    <label class="relative inline-flex items-center cursor-pointer">
                      <input
                        type="checkbox"
                        checked={skill.enabled}
                        onchange={() => handleToggle(skill.skill_id, !skill.enabled)}
                        disabled={actionInFlight === skill.skill_id}
                        class="sr-only peer"
                      />
                      <div class="w-9 h-5 rounded-full bg-mpx-muted/30
                                  peer-checked:bg-mpx-orange
                                  peer-disabled:opacity-50
                                  after:content-[''] after:absolute after:top-0.5 after:left-[2px]
                                  after:bg-white after:rounded-full after:h-4 after:w-4
                                  after:transition-all
                                  peer-checked:after:translate-x-4
                                  transition-colors">
                      </div>
                    </label>
                  </div>
                </div>
              </div>
            {/each}

          {/if}
        </div>
      {/if}
    </div>
  {/if}
</div>
