# Audio performance benchmark record

No performance or synchronization number belongs here until it has been measured on the running executable. The initial baseline is intentionally empty.

| Build / hardware | Outputs | Mode | Start-to-audio | Steady software latency | Group sync error | Drift | CPU | Memory | Underruns / overruns | Disconnect recovery | Reconnect recovery |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Not yet built | — | — | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured |

## Measurement protocol

- Record each endpoint’s negotiated format, engine period, render padding, device clock position, target buffer, and correction ratio.
- Measure process CPU and private working set with Windows Performance Recorder/Analyzer or a documented equivalent.
- Distinguish software timing from acoustic timing. Acoustic timing must record the microphones, placement, and analysis method.
- Run at least 10 minutes steady-state and three disconnect/reconnect cycles per configuration.
- Record external CPU/GPU/network load and power state. Do not interpolate missing hardware measurements.

