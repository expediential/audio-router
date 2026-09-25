# SyncAudio (engineering prototype)

SyncAudio is a native Windows 11 audio-sharing utility under active development. It is designed to capture the shared system mix and independently render it to up to five selected Windows audio endpoints. The product deliberately does **not** install a virtual or kernel driver, modify the default endpoint, record audio, or send telemetry.

## Current status

The repository currently contains the research and native audio-core foundation (Milestone 1 plus the first real device-management component). `syncaudio-cli` enumerates actual active render endpoints through the Windows MMDevice API; no device list is fabricated. Multi-device loopback rendering and the desktop UI are subsequent milestones and are not represented as completed features.

## What is technically possible

- Wired, USB, HDMI/DisplayPort, and Bluetooth endpoints appear as Windows render endpoints and can be selected in a five-device group.
- WASAPI shared-mode loopback can capture the mix sent to one Windows render endpoint; individual shared-mode render clients can feed the other endpoints.
- Each render pipeline can have its own format conversion, bounded buffer, gain, manual delay, clock observations, recovery state, and drift correction.
- The router can estimate and compensate software/device-buffer timing. It cannot infer the acoustic delay inside arbitrary Bluetooth earbuds without an external measurement path (for example, optional microphone-assisted calibration).

## Honest latency expectations

“Low latency” means the smallest reliable Windows engine period and bounded buffering the selected endpoints support. It does not mean zero Bluetooth latency. A Bluetooth endpoint can add codec, radio, firmware, and DSP latency that is outside the public Windows endpoint clock. SyncAudio will delay faster outputs to match known or manually calibrated slower outputs, and will surface uncertainty rather than claim physical sample-perfect sync.

On Windows 11, opening a Bluetooth headset microphone can switch that headset from A2DP media playback to HFP communications mode. This may reduce it to mono 8/16 kHz operation. The finished app will detect format changes and warn rather than conceal the change.

## Architecture decision

The engine is C++20 with direct Core Audio/WASAPI calls, not a managed wrapper. This gives the render path exact access to endpoint buffers, clocks, event handles, MMCSS priorities, `IAudioClient3` periods, and device-invalidated errors. The planned desktop shell is WinUI 3, communicating with the engine through a narrow control API; it is intentionally not in the audio callback path.

See [the architecture decision](docs/architecture/architecture-decision.md) and [Windows audio architecture](docs/research/windows-audio-architecture.md).

## Build prerequisites

- Windows 11 x64
- Visual Studio 2022 Build Tools or Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.25 or later
- Windows 11 SDK

```powershell
cmake -S . -B out/build -A x64
cmake --build out/build --config Release
.\out\build\Release\syncaudio-cli.exe
```

The CLI only enumerates devices at this stage. It is safe to run: it does not start capture, open render streams, or alter endpoint configuration.

## Planned milestones

1. Research and API decision — complete.
2. Loopback capture and one isolated renderer.
3. Two-to-five endpoint fan-out with bounded buffers and recovery.
4. Clock observation, drift compensation, and manual calibration.
5. Native desktop shell, tray, profiles, diagnostics, and installer.

## Privacy and permissions

No administrator privilege is required for the user-mode WASAPI design. System audio is processed only in memory. Optional microphone-assisted calibration, if implemented, will be off by default and require an explicit user action.

## License

License selection is pending before external distribution. Third-party projects in the research document are studied for ideas only; no third-party source is incorporated.

