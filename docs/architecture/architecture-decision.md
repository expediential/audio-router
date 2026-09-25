# Architecture decision: native WASAPI core with a separated desktop shell

Status: accepted for Milestones 2–4 on 2026-09-25.

## Decision matrix

| Criterion | A — C++ / Core Audio / WinUI 3 | B — C# / NAudio / WPF or WinUI | C — Rust engine / Windows desktop UI |
| --- | --- | --- | --- |
| Render-buffer, clock, period access | Direct and complete | Available through wrappers, occasionally requires interop | Direct through bindings, with binding maturity risk |
| Real-time allocation/control discipline | Explicit, predictable | Good but GC and wrapper behavior must be carefully contained | Strong ownership model, but Windows audio binding work remains |
| Windows audio debugging/sample availability | Best | Strong, but abstraction can hide HRESULT/format details | Moderate |
| Native Windows 11 integration | Best with WinUI 3 shell | Strong | Requires more UI integration choices |
| Installer footprint | Small native binary plus UI runtime | Self-contained publish is convenient but larger | Moderate |
| Team maintainability | Requires Windows/C++ discipline | Easiest for many app teams | Good for Rust-specialist team only |

## Chosen architecture

Use C++20 and the Windows Core Audio APIs for the real-time engine. Use a future WinUI 3 C++/WinRT dashboard as a separate control client. The engine can initially live in-process, but all APIs are shaped as a control boundary so it can later move into a supervised user-mode broker without changing DSP ownership.

We do **not** use a service or kernel driver in the initial product. System-loopback capture and shared-mode endpoint rendering are supported by user-mode Core Audio. A future virtual endpoint would be a separately signed and security-reviewed product component, not a hidden dependency.

## Module boundaries

```text
WinUI 3 shell / tray ─┐
Profiles + diagnostics ── control messages ── Router coordinator
                                             │
Default render loopback ─ Capture worker ─ Master float timeline
                                             ├─ Endpoint worker 1 ─ WASAPI renderer
                                             ├─ Endpoint worker 2 ─ WASAPI renderer
                                             ├─ Endpoint worker 3 ─ WASAPI renderer
                                             ├─ Endpoint worker 4 ─ WASAPI renderer
                                             └─ Endpoint worker 5 ─ WASAPI renderer
```

Each endpoint worker owns one bounded ring buffer, negotiated device format, resampler, gain/mute, latency delay line, stream clock measurements, and recovery state. The shared capture worker never waits for rendering workers. `SyncEngine` operates on timestamps/metrics and produces bounded, smooth correction commands; it does not directly mutate COM objects from a UI thread.

## Operating modes

Quality, Balanced, and Low Latency map to measured candidates from `IAudioClient3` / actual stable stream configuration. “Ultra low latency” remains hidden until a device supports and sustains it. Modes modify target buffer depth and allowed engine period, never promise a period the endpoint has not accepted.

## Non-goals for the first playable core

- Bluetooth codec selection or forcing A2DP while another application requires HFP.
- Per-application capture/routing, virtual audio endpoints, ASIO, and remote streaming.
- Claiming automatic physical/acoustic calibration without an explicit external measurement method.

