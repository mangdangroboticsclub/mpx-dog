<script>
  let { navigate, apIp = "192.168.2.1", staState = "disconnected", staSsid = "", staIp = "" } = $props();

  const menu = [
    { id: "control", icon: "🎮", label: "Control",      desc: "D-pad & gait control" },
    { id: "lua",     icon: "🌙", label: "Lua Editor",   desc: "Write & run Lua scripts" },
    { id: "chat",    icon: "💬", label: "Chat",         desc: "Conversation interface" },
    { id: "wifi",    icon: "📶", label: "WiFi",         desc: "Connect robot to LAN" },
    { id: "skills",  icon: "⚡", label: "Skills",       desc: "Run .wasm skill files" },
    { id: "files",   icon: "📁", label: "File Viewer",  desc: "Browse LittleFS storage" },
    { id: "upload",  icon: "📤", label: "Upload Skill", desc: "Upload .wasm to robot" },
  ];

  // ── Viewport / magnification / pagination state ─────────────
  let zoomLevel = $state(1);
  let itemsPerPage = $state(6);
  let currentPage = $state(0);
  let scrollEl = $state(null);
  let containerEl = $state(null);

  // Derive pages array reactively
  let pages = $derived.by(() => {
    const p = [];
    for (let i = 0; i < menu.length; i += itemsPerPage) {
      p.push(menu.slice(i, i + itemsPerPage));
    }
    return p;
  });

  /**
   * Calculate how many menu items fit per page based on the
   * available container height and the current magnification.
   *
   * Magnification (pinch-zoom / browser zoom) is captured by
   * `window.visualViewport.scale`.  The container's `clientHeight`
   * already shrinks when zoomed, so we divide the card-row height
   * estimate by the scale to get the effective row cost.
   */
  function calcItemsPerPage() {
    if (!containerEl) return;
    const vv = window.visualViewport;
    const scale = vv?.scale ?? 1;
    zoomLevel = scale;

    const availH = containerEl.clientHeight;

    // Estimated height of one card row (2 cols) at 1x scale:
    //   icon (text-2xl ≈ 28px) + label (~14px) + desc (~12px)
    //   + padding (py-4 = 32px) + gap (gap-3 = 12px) ≈ 98px
    // Round up to 110px for border & safety margin.
    const ROW_AT_1X = 110;
    const GAP = 12;
    const rowCost = ROW_AT_1X + GAP;

    // Scale‑adjusted: at 2× zoom a 110px row costs 220px of viewport
    const effectiveRowH = rowCost * scale;

    const rows = Math.max(1, Math.floor((availH - 16) / effectiveRowH));
    itemsPerPage = Math.max(2, rows * 2);
  }

  /** Synchronise `currentPage` with the scroll‑snap position. */
  function onScroll() {
    if (!scrollEl) return;
    const idx = Math.round(scrollEl.scrollLeft / scrollEl.clientWidth);
    if (idx !== currentPage) currentPage = idx;
  }

  /** Scroll to a specific page (called from dot indicators). */
  function goToPage(idx) {
    if (!scrollEl) return;
    scrollEl.scrollTo({
      left: idx * scrollEl.clientWidth,
      behavior: "smooth",
    });
    currentPage = idx;
  }

  // ── React to viewport / resize changes ──────────────────────
  $effect(() => {
    calcItemsPerPage();

    // Observe container size changes (keyboard open/close, rotation…)
    const ro = new ResizeObserver(calcItemsPerPage);
    if (containerEl) ro.observe(containerEl);

    // visualViewport fires resize/scroll on pinch‑zoom
    const vv = window.visualViewport;
    const onVvChange = () => calcItemsPerPage();
    if (vv) {
      vv.addEventListener("resize", onVvChange);
      vv.addEventListener("scroll", onVvChange);
    }
    window.addEventListener("resize", onVvChange);

    // Also re‑calc when CSS `zoom` changes via media query or
    // browser‑level default zoom (Firefox, Chrome settings).
    const zoomMq = window.matchMedia("(min-resolution: 1dppx)");
    // (resolution queries fire when effective zoom changes)
    const onMq = () => calcItemsPerPage();
    zoomMq.addEventListener("change", onMq);

    return () => {
      ro.disconnect();
      if (vv) {
        vv.removeEventListener("resize", onVvChange);
        vv.removeEventListener("scroll", onVvChange);
      }
      window.removeEventListener("resize", onVvChange);
      zoomMq.removeEventListener("change", onMq);
    };
  });

  // Keep currentPage in bounds when pages count changes
  $effect(() => {
    const n = pages.length;
    if (currentPage >= n) currentPage = n - 1;
  });

  // ── Derived helpers ─────────────────────────────────────────
  function staDot() {
    switch (staState) {
      case "connected":    return "bg-green-400";
      case "connecting":   return "bg-yellow-400";
      case "failed":       return "bg-red-500";
      default:             return "bg-gray-500";
    }
  }

  function staLabel() {
    switch (staState) {
      case "connected":    return staIp || "Connected";
      case "connecting":   return "Connecting…";
      case "failed":       return "Failed";
      default:             return "not connected";
    }
  }
</script>

<div class="flex flex-col h-full select-none" bind:this={containerEl}>
  <!-- Logo / Title -->
  <div class="flex flex-col items-center gap-1 pt-3 pb-1 shrink-0">
    <span class="text-4xl">◆</span>
    <h1 class="text-lg font-bold tracking-wide text-mpx-text">MPX-Dog</h1>
  </div>

  <!-- Swipeable Pages (horizontal scroll-snap) -->
  <div
    class="flex-1 overflow-x-auto overflow-y-hidden snap-x snap-mandatory scroll-smooth hide-scrollbar"
    style="overscroll-behavior-x: contain; -webkit-overflow-scrolling: touch;"
    bind:this={scrollEl}
    onscroll={onScroll}
  >
    <div class="flex h-full px-3">
      {#each pages as page, i}
        <!-- svelte-ignore a11y_no_static_element_interactions -->
        <div
          class="snap-start min-w-full h-full flex flex-col justify-center {i > 0 ? 'pl-3' : ''}"
        >
          <div class="grid grid-cols-2 gap-3 w-full max-w-sm mx-auto">
            {#each page as item}
              <button
                onclick={() => navigate(item.id)}
                class="flex flex-col items-center gap-1 rounded-xl bg-mpx-surface
                       border border-mpx-muted/10 px-2 py-4
                       hover:border-mpx-orange/50 hover:bg-mpx-orange/5
                       active:border-mpx-orange active:scale-[0.97]
                       transition-all cursor-pointer touch-manipulation"
              >
                <span class="text-2xl leading-none">{item.icon}</span>
                <span class="font-semibold text-xs text-mpx-text text-center leading-tight">{item.label}</span>
                <span class="text-[10px] text-mpx-muted text-center leading-tight">{item.desc}</span>
              </button>
            {/each}
            <!-- Fill empty slots so sparse pages keep alignment -->
            {#each Array(itemsPerPage - page.length) as _}
              <div class="invisible pointer-events-none" aria-hidden="true"></div>
            {/each}
          </div>
        </div>
      {/each}
    </div>
  </div>

  <!-- Page Indicators (iOS‑style dots) -->
  {#if pages.length > 1}
    <div class="flex items-center justify-center gap-2 py-2 shrink-0" role="tablist" aria-label="App pages">
      {#each pages as _, i}
        <button
          role="tab"
          aria-selected={i === currentPage}
          aria-label="Page {i + 1} of {pages.length}"
          onclick={() => goToPage(i)}
          class="h-2 rounded-full transition-all duration-300 cursor-pointer touch-manipulation
                 {i === currentPage
                   ? 'w-5 bg-mpx-orange'
                   : 'w-2 bg-mpx-muted/30 hover:bg-mpx-muted/50'}"
        ></button>
      {/each}
    </div>
  {/if}

  <!-- Connection Status -->
  <div class="flex items-center justify-center gap-2 pb-3 shrink-0 text-xs text-mpx-muted">
    <span class="flex items-center gap-1">
      <span class="w-2 h-2 rounded-full bg-green-400 shrink-0"></span>
      AP: {apIp}
    </span>
    <span class="text-mpx-muted/30">·</span>
    <span class="flex items-center gap-1">
      <span
        class="w-2 h-2 rounded-full shrink-0 {staDot()}
               {staState === 'connecting' ? 'animate-pulse' : ''}"
      ></span>
      STA: {staLabel()}
    </span>
    {#if zoomLevel !== 1}
      <span class="text-mpx-muted/40">·</span>
      <span class="text-[10px] text-mpx-orange/70">{zoomLevel.toFixed(1)}×</span>
    {/if}
  </div>
</div>
