# WineForge Userspace Sync (WFUSync)

**WineForge Userspace Sync (WFUSync)** is a macOS-focused userspace synchronization backend for Wine.

The original Wine-NTsync userspace macOS prototype did not prove reliable enough for real-world use. Development therefore moved to WFUSync, a new correctness-first implementation designed specifically for Wine on macOS.

WFUSync follows the same general goal as NTSync, MSync, FSync, and ESync: reducing Wine synchronization overhead. It is an independent WineForge implementation and does not attempt to copy another backend 1:1.

> **Status:** WFUSync v5 is currently a testing release of an experimental backend. Correctness and stress tests pass, but application-specific problems may still exist. Unsupported or sensitive synchronization paths intentionally fall back to the Wine server.

---

## Current Status

WFUSync v5 is tested with **WineForge / Wine 11.12** on macOS.

Implemented:

- Event wait, set, reset, and query
  - auto-reset and manual-reset Events
  - named Event support
- Semaphore wait and release
  - named Semaphore support
  - Semaphore query intentionally falls back to the Wine server
- Mutex wait, release, query, recursion, and abandonment
  - named and unnamed Mutex support
- WaitAny for supported Event, Semaphore, and Mutex combinations
- Atomic userspace WaitAll for supported auto-reset Event, Semaphore, and Mutex combinations
- Correctness-safe server-assisted WaitAll for manual-reset, contended, or unsupported cases
- POSIX shared memory using `shm_open()` followed immediately by `shm_unlink()`
- `os_sync_wait_on_address` and `os_sync_wake_by_address` on newer macOS
- `__ulock_wait` and `__ulock_wake` fallback on older macOS
- `os_unfair_lock` for the macOS client cache

Enable WFUSync with:

```bash
WINEWFUSYNC=1
```

---

Stress results include:

```text
WaitAll registration stress: 1000 iterations, 0 failures
Competing WaitAll stress: 200 rounds, 400 waits, 0 failures
```

---

## Microbenchmark Snapshot

Local macOS microbenchmark, 1,000,000 operations per scenario:

| Scenario | WFUSync v5 | CrossOver MSync |
|---|---:|---:|
| Auto Event set/wait | 0.000580 ms/op | 0.000666 ms/op |
| Semaphore release/wait | 0.000607 ms/op | 0.000683 ms/op |
| Mutex wait/release | 0.000625 ms/op | 0.000686 ms/op |
| WaitAny auto Events | 0.000627 ms/op | 0.000704 ms/op |
| WaitAll auto Events | 0.001186 ms/op | 0.001291 ms/op |
| WaitAll Event + Semaphore | 0.001183 ms/op | 0.001288 ms/op |
| WaitAll Event + Mutex | 0.001192 ms/op | 0.001294 ms/op |

These are synthetic local microbenchmark results, not independently reproduced application benchmarks. Results can be affected by hardware, macOS version, Wine build, prefix state, process startup, scheduling, and benchmark design. Lower synchronization latency does not guarantee higher FPS or faster performance in every game or application.

Real-world performance depends on whether synchronization is an actual bottleneck. GPU-bound workloads may show little or no difference.
