# Documentation

| Document | What it is |
|---|---|
| [`architecture.md`](architecture.md) | Data model, component structure, event-log format (v1–v3), invariants, design decisions |
| [`workflow.md`](workflow.md) | The per-change loop, definition of done, benchmark protocol, profiling recipes |
| [`benchmark-log.md`](benchmark-log.md) | Append-only table of measured results, each with commit, hardware, flags and run count |
| [`guides/setup.md`](guides/setup.md) | Install, build, verify, tool usage and troubleshooting (Linux / WSL2) |
| [`guides/native-linux-checklist.md`](guides/native-linux-checklist.md) | Measurements that need native Linux, their status, and the WSL2 baseline |
| [`guides/native-linux-live-usb.md`](guides/native-linux-live-usb.md) | Operator manual for the native runs from a live USB |

Where to start: `architecture.md` for how it works, and `benchmark-log.md` for the evidence behind any number in the top-level README.
