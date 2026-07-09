# WineForge Userspace Sync (WFUSync)

**WineForge Userspace Sync (WFUSync)** is an experimental userspace synchronization backend for Wine on macOS.

The original **Wine-NTsync userspace macOS prototype** did not prove reliable enough for real-world use. It was based on the idea of providing NT-style synchronization primitives without a Linux `ntsync` kernel driver, but the first approach was too limited and not suitable for stable game/application testing.

Because of that, development has moved to **WFUSync**, a new WineForge-focused implementation.

WFUSync is inspired by the same goal as Linux `ntsync`, MSync, FSync, and ESync: reducing synchronization overhead in Wine workloads. However, WFUSync is designed specifically for macOS and follows a conservative correctness-first model.

> ⚠️ **Disclaimer**: WFUSync is experimental. It is currently being tested in WineForge-based Wine builds and may still have bugs, missing hot paths, or application-specific issues. Use at your own risk.

---

## Current Status

WFUSync is currently tested with **WineForge / Wine 11.12** on macOS.

Implemented so far:

- Single-object Event hot path
  - auto-reset Event wait/set/reset/query
  - manual-reset Event wait/set/reset/query
- Single-object Semaphore hot path
  - wait/release/query
- Single-object Mutex hot path
  - wait/release/query
- WaitAny fast path for supported Event/Semaphore/Mutex cases
- Limited WaitAll support
  - currently handled through a correctness-safe server-arbitrated path
  - not a full client-only hot path
  - complex or uncertain cases may still fall back to the Wine server
- Named Event/Semaphore shared-state support for supported cases
  - not all named-object/server-waiter/unsupported cases are forced into the hot path
- macOS address-wait backend
  - `os_sync_wait_on_address` / `os_sync_wake_by_address` on newer macOS
  - `__ulock_wait` / `__ulock_wake` fallback
- macOS cache locking uses `os_unfair_lock`

Runtime gate:

```bash
WINEWFUSYNC=1
