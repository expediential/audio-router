# Audio routing ecosystem research

Research date: 2026-09-25. This is a design study, not a code-import plan. No source from these projects is included in SyncAudio. Repository metadata and licenses must be rechecked at the exact revision before any future reuse.

| Project | Architecture | Useful components / ideas | Problems / limits | License | What we learn |
| --- | --- | --- | --- | --- | --- |
| [Synchronous Audio Router](https://github.com/eiz/SynchronousAudioRouter) | C++ user tooling around a Kernel Streaming virtual audio driver; includes ASIO routing | Mature virtual-endpoint/routing concept, explicit buffers and endpoint model | Kernel driver installation/signing and GPL obligations are incompatible with the first no-driver product path; it solves a broader virtual-routing problem than endpoint mirroring | GPL-3.0 | Keep the initial product user-mode only. A virtual driver is a separately governed future decision, not a shortcut. |
| [EchoBridge](https://github.com/atreyakamat/echobridge) | C#/.NET 8 WPF, NAudio loopback, fan-out and per-output FX | Product decomposition: capture, router, output object, per-device controls | Very early/small project; README claims are not hardware validation; FX is outside first fidelity-first path | MIT | A simple loopback-to-fan-out model is approachable, but implementation must measure buffer states and isolate failures. |
| [AudioHQ](https://github.com/UnderFusion/AudioHQ) | C# WPF + NAudio; loopback capture, per-output buffers and adaptive resampling | Explicit endpoint isolation, settings recovery, adaptive drift based on buffer occupancy, test coverage | Managed wrappers hide some Core Audio controls; claimed behavior still requires independent hardware validation; supports unbounded outputs rather than product’s five-device guardrail | MIT | Buffer-occupancy feedback plus a tightly bounded resample ratio is the right baseline for clock correction. |
| [SoundSync](https://github.com/sugumar247/SoundSync) | C#/.NET WPF + NAudio, default-device loopback and selected output renderers | System mix capture, live device recovery, endpoint-specific volume/delay intent | No proof that physical Bluetooth latency is measurable; includes network streaming, which is deliberately out of SyncAudio scope | MIT | A non-driver local endpoint mirror is viable; source selection/recovery needs careful default-device handling. |
| [Double Headphones](https://github.com/maayaranai/double-headphones) | Consumer-oriented no-driver Windows audio sharing product | Clear simple flow and an honest manual delay-control UX | Public repository is currently documentation-heavy; no source architecture to assess, and “in sync” requires hardware-specific qualification | License shown in repository; verify before reuse | Keep the default flow simple but never collapse uncertain Bluetooth timing into a false certainty. |
| [AudioRouter](https://github.com/ViggoLarsen/AudioRouter) | Rust Windows service with YAML-configured WASAPI routing | Rust approach and configuration-driven routing are useful comparison points | Service increases lifecycle/permission/cleanup burden; repository is small and does not establish multi-endpoint synchronization | License shown in repository; verify before reuse | A service is unnecessary for the first desktop product; process-scoped cleanup is safer and easier to audit. |

## Adjacent primary references

- Microsoft’s [WASAPI overview](https://learn.microsoft.com/windows/win32/coreaudio/wasapi) defines the client, render, capture, clock, session, and `IAudioClient3` interfaces used by the engine.
- Microsoft’s [loopback recording guidance](https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording) confirms loopback is shared-mode only and, on Windows 10 1703+, supports event-driven loopback capture.
- Microsoft’s [Bluetooth Classic Audio documentation](https://learn.microsoft.com/windows-hardware/drivers/bluetooth/bluetooth-classic-audio) describes A2DP/HFP behavior and the Windows 11 unified endpoint behavior.
- Microsoft’s [device-events documentation](https://learn.microsoft.com/windows/win32/coreaudio/device-events) specifies `IMMNotificationClient` endpoint-change notifications.

## Reuse policy

We may study public behavior and independently implement concepts. Copying GPL driver code would make the combined work subject to GPL obligations, so it is prohibited for this repository. MIT projects can technically be reused with notices, but SyncAudio currently incorporates no third-party source. Any later adoption requires: pinned commit, license review, copyright notice preservation, security review, and a test that establishes the use is needed.

## Additional ecosystem findings

Windows’ own “Listen to this device” can mirror a capture endpoint to one render path, but it is not a suitable multi-render synchronization engine. Virtual cable/mixer products can solve application-routing problems, but introduce an installable driver and change the risk/installer model. SyncAudio therefore begins with a no-driver user-mode mirror of a render endpoint’s shared mix.

The combination “five unrelated Bluetooth earbuds” can be radio-limited on a particular adapter. The router must treat a device not reaching stable render state as an endpoint-local failure, not as evidence that all selected endpoint types are unsupported.

