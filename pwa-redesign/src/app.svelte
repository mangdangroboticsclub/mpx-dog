<script>
  import LockScreen from "./lib/LockScreen.svelte";

  // ── Network status (polled from robot) ──────────────────────
  let apIp = $state("192.168.2.1");
  let apSsid = $state("MPX-Dog");
  let staState = $state("disconnected");
  let staSsid = $state("");
  let staIp = $state("");
  let networkLoaded = $state(false);

  let pollTimer;

  async function pollNetworkStatus() {
    try {
      const res = await fetch("/v1/wifi/status");
      if (res.ok) {
        const data = await res.json();
        apIp = data.ap?.ip ?? "192.168.2.1";
        apSsid = data.ap?.ssid ?? "MPX-Dog";
        staState = data.sta?.state ?? "disconnected";
        staSsid = data.sta?.ssid ?? "";
        staIp = data.sta?.ip ?? "";
        networkLoaded = true;
      }
    } catch {
      // Robot unreachable — keep last known state
    }
  }

  // ── Poll on mount, refresh every 15 s ───────────────────────
  $effect(() => {
    pollNetworkStatus();
    pollTimer = setInterval(pollNetworkStatus, 15_000);
    return () => clearInterval(pollTimer);
  });

  // ── Pack props for children ─────────────────────────────────
  let network = $derived({ apIp, apSsid, staState, staSsid, staIp, networkLoaded });
</script>

<LockScreen {network} />
