<script>
  let { navigate } = $props();

  let skills = $state([]);
  let loading = $state(true);
  let running = $state(null);
  let runOutput = $state("");

  async function fetchSkills() {
    loading = true;
    try {
      const res = await fetch("/v1/skills/list");
      if (res.ok) {
        skills = await res.json();
      } else {
        skills = [];
      }
    } catch {
      skills = [];
    }
    loading = false;
  }

  async function runSkill(name) {
    running = name;
    runOutput = "";
    try {
      const res = await fetch("/v1/skills/run", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ skill: name }),
      });
      if (res.ok) {
        const data = await res.json();
        runOutput = data.output || "Executed successfully";
      } else {
        runOutput = `Error: ${res.status}`;
      }
    } catch (e) {
      runOutput = `Failed: ${e.message}`;
    }
    running = null;
  }

  $effect(() => { fetchSkills(); });
</script>

<div class="flex flex-col h-full">
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20">
    <button onclick={() => navigate("home")}
            class="text-lg hover:text-mpx-orange transition-colors cursor-pointer">‹</button>
    <h2 class="font-semibold">Skills</h2>
    <button onclick={fetchSkills}
            class="ml-auto text-xs text-mpx-muted hover:text-mpx-text transition-colors cursor-pointer">
      ↻ Refresh
    </button>
  </header>

  <div class="flex-1 overflow-y-auto px-4 py-3">
    {#if loading}
      <p class="text-center text-mpx-muted text-sm mt-8">Loading skills…</p>
    {:else if skills.length === 0}
      <p class="text-center text-mpx-muted text-sm mt-8">
        No skill files found on the robot.
      </p>
      <p class="text-center text-xs text-mpx-muted mt-2">
        Upload a .wasm file from the Upload page.
      </p>
    {:else}
      <div class="space-y-2">
        {#each skills as skill}
          <div class="flex items-center justify-between rounded-lg bg-mpx-surface
                      border border-mpx-muted/10 px-4 py-3">
            <div class="flex items-center gap-3">
              <span class="text-lg">⚡</span>
              <div>
                <p class="text-sm font-medium text-mpx-text">{skill.name}</p>
                <p class="text-xs text-mpx-muted">{(skill.size / 1024).toFixed(1)} KB</p>
              </div>
            </div>
            <button
              onclick={() => runSkill(skill.name)}
              disabled={running === skill.name}
              class="rounded-lg bg-mpx-orange px-4 py-1.5 text-xs text-white
                     hover:bg-mpx-orange-light transition-colors cursor-pointer
                     {running === skill.name ? 'opacity-50 animate-pulse' : ''}"
            >
              {running === skill.name ? "Running…" : "Run"}
            </button>
          </div>
        {/each}
      </div>
    {/if}

    {#if runOutput}
      <div class="mt-4 rounded-lg bg-black/40 border border-mpx-muted/20 px-4 py-3">
        <p class="text-xs text-mpx-muted mb-1">Output:</p>
        <pre class="text-xs text-green-400 font-mono whitespace-pre-wrap">{runOutput}</pre>
      </div>
    {/if}
  </div>
</div>
