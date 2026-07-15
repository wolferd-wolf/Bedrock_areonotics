# Runtime observations

## 2026-07-15 — Milestone 2A heartbeat probe

Target-device observations for build `0.0.2-dev+b8c0297`:

- Compatibility profile accepted.
- GNU build ID: `2e318db12824cadb2618754ab7c82fa96fb30659`.
- `libminecraftpe.so` size: `349243744` bytes.
- Resolved callback offset: `0x9d80fac`.
- Hook installation completed without a crash.
- No Info-level heartbeat samples appeared while the player remained in-world for at least 30 seconds.
- The LeviLauncher overlay was filtered to level `I`; the original zero-count diagnostic used Warning level and was therefore hidden.

Interpretation:

- The hook and compatibility gate are working.
- The selected `isShowingMenu` callback is not yet proven to execute continuously during normal in-world gameplay.
- This target must not be described as the authoritative game tick.
- The diagnostic build was updated to emit zero-count samples at Info level and to announce sampler-thread startup.
