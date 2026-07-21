<script>
  let { navigate } = $props();

  let dragging = $state(false);
  let uploading = $state(false);
  let uploadStatus = $state("");
  let selectedFile = $state(null);
  let dragRef = $state(null);

  function onDragOver(e) {
    e.preventDefault();
    dragging = true;
  }

  function onDragLeave() {
    dragging = false;
  }

  function onDrop(e) {
    e.preventDefault();
    dragging = false;
    if (e.dataTransfer.files.length > 0) {
      selectedFile = e.dataTransfer.files[0];
    }
  }

  function onFileSelect(e) {
    if (e.target.files.length > 0) {
      selectedFile = e.target.files[0];
    }
  }

  async function upload() {
    if (!selectedFile) return;
    uploading = true;
    uploadStatus = "Uploading…";

    try {
      const name = encodeURIComponent(selectedFile.name);
      const res = await fetch(`/v1/skills/upload?name=${name}`, {
        method: "POST",
        body: await selectedFile.arrayBuffer(),
      });

      if (res.ok) {
        uploadStatus = `✅ ${selectedFile.name} uploaded successfully`;
        selectedFile = null;
      } else {
        const err = await res.text();
        uploadStatus = `❌ Upload failed: ${err}`;
      }
    } catch (e) {
      uploadStatus = `❌ Error: ${e.message}`;
    }
    uploading = false;
  }

  function formatSize(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
</script>

<div class="flex flex-col h-full">
  <header class="flex items-center gap-3 px-4 py-3 bg-mpx-surface border-b border-mpx-muted/20">
    <button onclick={() => navigate("home")}
            class="text-lg hover:text-mpx-orange transition-colors cursor-pointer">‹</button>
    <h2 class="font-semibold">Upload Skill</h2>
  </header>

  <div class="flex-1 flex flex-col items-center justify-center gap-4 px-4">
    <!-- Drop zone -->
    <div
      bind:this={dragRef}
      ondragover={onDragOver}
      ondragleave={onDragLeave}
      ondrop={onDrop}
      class="w-full max-w-sm rounded-xl border-2 border-dashed px-6 py-12
             text-center transition-colors cursor-pointer
             {dragging
               ? 'border-mpx-orange bg-mpx-orange/5'
               : 'border-mpx-muted/30 hover:border-mpx-muted/50'}"
      onclick={() => dragRef?.querySelector("input").click()}
      role="button"
      tabindex="0"
      onkeydown={(e) => e.key === "Enter" && dragRef?.querySelector("input").click()}
    >
      <input
        type="file"
        accept=".wasm,.mpxe"
        onchange={onFileSelect}
        class="hidden"
      />
      {#if selectedFile}
        <span class="text-4xl">📄</span>
        <p class="mt-2 text-sm text-mpx-text font-medium">{selectedFile.name}</p>
        <p class="text-xs text-mpx-muted">{formatSize(selectedFile.size)}</p>
      {:else}
        <span class="text-4xl">📤</span>
        <p class="mt-2 text-sm text-mpx-text">
          {dragging ? "Drop it here!" : "Drag & drop a .wasm or .mpxe file, or tap to browse"}
        </p>
        <p class="text-xs text-mpx-muted mt-1">Max file size: 128 KB</p>
      {/if}
    </div>

    <!-- Upload button -->
    {#if selectedFile}
      <button
        onclick={upload}
        disabled={uploading}
        class="rounded-lg bg-mpx-orange px-8 py-2.5 text-sm text-white
               hover:bg-mpx-orange-light transition-colors cursor-pointer
               {uploading ? 'opacity-50' : ''}"
      >
        {uploading ? "Uploading…" : "Upload to Robot"}
      </button>
    {/if}

    <!-- Status -->
    {#if uploadStatus}
      <p class="text-sm text-center max-w-sm {uploadStatus.includes('✅') ? 'text-green-400' : 'text-red-400'}">
        {uploadStatus}
      </p>
    {/if}
  </div>
</div>
