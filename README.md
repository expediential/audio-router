# SyncAudio (engineering prototype)

SyncAudio is a native Windows 11 audio-sharing utility under active development. It is designed to capture the shared system mix and independently render it to up to five selected Windows audio endpoints. The product deliberately does **not** install a virtual or kernel driver, modify the default endpoint, record audio, or send telemetry.

## Current status

The repository contains the research and native audio-core foundation, a native Windows dashboard, actual MMDevice endpoint enumeration, and functional WASAPI output routes. Double-click `SyncAudio.exe` to choose up to five active destination endpoints, adjust their real Windows endpoint volume/mute state, refresh their current connection state, and start isolated WASAPI loopback routes. The new dashboard is a single responsive, custom-rendered light/neumorphic interface: device cards, controls, scrolling, resizing, and DPI scaling all use one visual system, so it avoids the native-control paint corruption present in earlier builds. The dashboard never invents device data or successful routing states.

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

## Dashboard use

1. Open `SyncAudio.exe`.
2. Wait for the app to mark an output **Ready**. It preflights the real Windows audio route; disconnected endpoints and formats the current engine cannot yet route remain visibly unavailable. The Windows default endpoint remains the system-audio source and cannot be selected as a destination.
3. Tap a device card to add or remove it from the group and to use its real endpoint-volume slider or mute control.
4. Select **START AUDIO SHARING**. Each destination is started in its own WASAPI worker so a failed device does not stop the rest.

The dashboard automatically refreshes device readiness while it is open. Its current renderer requires matching Float32 shared-mode sample rate and channel count. Format adapters, shared-capture fan-out, and drift-controlled synchronization are active development work; the UI says this plainly rather than treating unrelated device clocks as synchronized.

`SyncAudio-cli.exe` remains a diagnostics tool: it lists exact endpoint IDs and can exercise a single explicit mirror route.

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

To run the dashboard from a local build:

```powershell
.\out\build\syncaudio-app.exe
```

To run a one-device CLI mirror, list endpoints and copy the complete ID of a non-default device, then pass it explicitly. The optional duration defaults to 60 seconds. Ctrl+C stops the route and releases both WASAPI streams.

```powershell
.\out\build\syncaudio-cli.exe
.\out\build\syncaudio-cli.exe --mirror '<destination endpoint ID>' 60
```

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
