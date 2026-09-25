# Multi-device audio test plan

Do not mark a combination as passed unless it ran on the listed physical hardware. “Not tested” is a valid result.

| Test | Devices | Expected Result | Actual Result | Notes |
| --- | --- | --- | --- | --- |
| Endpoint enumeration | Any active wired, USB, Bluetooth and HDMI endpoints | Friendly name, stable endpoint ID and state agree with Windows Sound settings | Not run | Requires compiled CLI and hardware. |
| One-output mirror | Default source + one non-source wired endpoint | Audible mirror, no capture feedback, stable render metrics for 30 min | Not run | |
| Mixed endpoints | One wired, one USB, one Bluetooth, one HDMI | All healthy endpoints continue independently; Bluetooth timing limitation shown | Not run | |
| Five outputs | Five distinct render endpoints | Router caps selection at five and isolates per-device failures | Not run | Radio capacity is hardware-specific. |
| Bluetooth disconnect | Active Bluetooth plus two wired endpoints | Only Bluetooth pipeline reconnects; wired audio continues | Not run | |
| Source default change | Loopback source switched in Windows | Capture pauses/rebuilds deliberately; no default endpoint mutation | Not run | |
| HFP transition | Bluetooth headset microphone opened during media | Format/profile warning, metrics capture format change, no false quality claim | Not run | |
| Drift soak | Two independent devices, 60 min | Sync error/buffer graphs stable; SRC ratio stays within configured bound | Not run | |
| Recovery | Kill router during playback | Router sessions stop and Windows source playback continues normally | Not run | |

## Procedure

Before each test record Windows build, audio driver versions, Bluetooth adapter, device firmware, sample-rate settings, selected mode, source application, CPU load, and all selected endpoint IDs. Capture diagnostic logs but never audio content. Use calibrated physical measurement equipment for claims about acoustic alignment.

