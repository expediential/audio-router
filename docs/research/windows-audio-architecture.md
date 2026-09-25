# Windows audio architecture for SyncAudio

## Scope and authoritative sources

The design uses Windows Core Audio / WASAPI directly. The authoritative references are Microsoft’s [Core Audio overview](https://learn.microsoft.com/windows/win32/coreaudio/about-the-windows-core-audio-apis), [WASAPI reference](https://learn.microsoft.com/windows/win32/coreaudio/wasapi), [loopback recording](https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording), [device formats](https://learn.microsoft.com/windows/win32/coreaudio/device-formats), and [Bluetooth Classic Audio](https://learn.microsoft.com/windows-hardware/drivers/bluetooth/bluetooth-classic-audio).

## API map

| API | SyncAudio use |
| --- | --- |
| `IMMDeviceEnumerator` / `IMMDeviceCollection` | Enumerate actual eRender endpoints and resolve default multimedia source. |
| `IMMNotificationClient` | Receive add/remove/state/default/property events; enqueue control-plane recovery work, never rebuild streams in the callback. |
| `IAudioClient` | Activate and initialize loopback capture and per-endpoint render streams; obtain mix format and buffer size. |
| `IAudioClient2` | Query stream capability/properties where useful; offload is not a first-release requirement. |
| `IAudioClient3` | Query shared engine periods and initialize the smallest proven-stable shared-mode period on Windows 10+. |
| `IAudioCaptureClient` | Pull frames from the loopback capture buffer. |
| `IAudioRenderClient` | Fill endpoint-owned render buffers after conversion/resampling. |
| `IAudioClock` / `IAudioClock2` | Observe endpoint stream position and correlate it with the master timeline. |
| `IAudioClockAdjustment` | Evaluated only when a stream supports it; software asynchronous resampling is the portable correction path. |
| `IAudioSessionControl` / `ISimpleAudioVolume` | Give router streams a clear session identity and session-level behavior; per-device gain remains in the endpoint signal path. |

## Shared versus exclusive mode

The initial design uses shared mode. WASAPI loopback is shared-mode only, and Windows 10+ `IAudioClient3` can offer small shared engine periods. Shared streams preserve the normal Windows mix, avoid taking exclusive control of a selected device, and work with Bluetooth endpoints. Exclusive mode is an opt-in future experiment: it can be lower latency on suitable hardware, but seizes an endpoint, creates recovery complexity, and cannot serve as the loopback source.

The source is the default multimedia render endpoint at start. The system audio app continues to play to that endpoint normally; SyncAudio mirrors that shared mix to selected **other** endpoints. Selecting the source again is rejected to avoid self-capture feedback.

## Threading and timing

1. An event-driven loopback capture thread reads frames and QPC-derived timestamps into the master fan-out.
2. The fan-out converts once into the internal 32-bit-float stereo representation and writes bounded copies to each active endpoint ring buffer.
3. One MMCSS-classed, event-driven render worker owns each endpoint’s `IAudioClient` / `IAudioRenderClient` and fills its buffer.
4. The control plane owns discovery, start/stop, recovery, profiles, diagnostics and UI. It never blocks the capture or render workers.

No render worker waits for another worker. A failed or slow endpoint gets its own state transition and recovery schedule. The capture thread applies a defined overflow policy and increments diagnostics; it never grows a buffer or blocks indefinitely.

## Format conversion, resampling, and quality

The master stream is 32-bit float. The capture format is converted once at ingress if required. Each renderer negotiates its own shared mix format using `GetMixFormat`; its adapter performs channel mapping and high-quality asynchronous SRC only when sample rate differs or drift correction is active. Gain happens before device-format quantization. Dither is used only on a deliberate float-to-integer conversion. No normalization, EQ, transcoding, or network transfer is in the initial routing path.

## Latency, clocks, jitter, and drift

`IAudioClock` and endpoint padding expose useful software-side timing, but not the final acoustic time of Bluetooth hardware. The master timeline records capture position and QPC time. For each endpoint, diagnostics track: target presentation time, device position, endpoint padding, software buffer depth, estimated software latency, configured manual offset, drift estimate, SRC ratio, sync error, and under/overruns.

Different render clocks will drift. The controller uses buffer occupancy and periodic clock observations to request a very small, rate-limited SRC ratio change (bounded initially to ±300 ppm). It must not use recurring sample drop/duplication as normal control. A serious underrun or resync is reported as a discontinuity.

Initial synchronization pre-fills all ready endpoints to an agreed target, starts them in controlled order, and delays faster paths to the slowest known target. Automatic calibration records only defensible software timing. Physical alignment requires manual adjustment or an explicitly enabled microphone-assisted procedure.

## Hot-plug and invalidation

`IMMNotificationClient` produces endpoint notifications. Stream operations can also return `AUDCLNT_E_DEVICE_INVALIDATED`. Either condition transitions only the affected endpoint to reconnecting/disconnected, tears down its COM objects on its owner thread, and attempts reconstruction with bounded backoff. The group remains active if at least one destination remains healthy. Source invalidation pauses capture and offers a deliberate source-recovery action; it never silently changes the user’s default device.

## Bluetooth and microphone behavior

A2DP is the normal high-quality stereo media path. HFP supports communications capture/playback and has lower bandwidth. On Windows 11 an accessory that supports both may expose one unified endpoint; opening its microphone or creating a Communications-category output can make Windows select HFP. Windows owns codec/profile selection and may resample internally. SyncAudio will show the endpoint’s observable current stream format and a quality warning on a format/profile-compatible change; it will not claim codec control or bit-perfect Bluetooth output.

## Buffer error handling

An underrun means an endpoint could not obtain enough frames in time: render silence for the deficit, count and timestamp it, retain other outputs, then progressively select a more stable period/buffer target if recurrence crosses policy. An overrun means a device pipeline is consuming too slowly: cap buffer depth, count it, correct drift gradually, and reset only that endpoint when safe. Neither condition is concealed as “locked.”

