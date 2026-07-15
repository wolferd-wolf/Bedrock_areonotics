# Runtime observations

## 2026-07-15 — Milestone 2A heartbeat proof

### Initial 0.0.2 probe

Target-device observations for build `0.0.2-dev+b8c0297`:

- Compatibility profile accepted.
- GNU build ID: `2e318db12824cadb2618754ab7c82fa96fb30659`.
- `libminecraftpe.so` size: `349243744` bytes.
- Resolved callback offset: `0x9d80fac`.
- Hook installation completed without a crash.
- No useful sampler lines appeared in LeviLauncher's live overlay.

Interpretation:

- The hook and compatibility gate were working.
- The live overlay was not reliable enough to prove callback activity.
- The selected callback was not treated as the authoritative game tick.

### Independent 0.0.3 diagnostic

Build `0.0.3-dev+14a77d1` added `heartbeat-status.txt`, first-callback logging, and two-second status snapshots independent of the live overlay.

First device result after normal shutdown:

```text
schema=1
state=stopped
sampler_sequence=0
timestamp_unix_ms=1784120199522
total_callbacks=14756
callbacks_since_previous=0
```

Final acceptance run after at least ten minutes in-world, leaving to the menu, re-entering the world, and exiting normally:

```text
schema=1
state=stopped
sampler_sequence=0
timestamp_unix_ms=1784121252752
total_callbacks=375298
callbacks_since_previous=0
```

Conclusions:

- The callback executed at least `375,298` times during the acceptance run.
- Original-function chaining remained stable.
- Minecraft remained stable for at least ten minutes in-world.
- Leaving the world, returning to the menu, and re-entering succeeded.
- Normal shutdown removed the hook without a crash.
- `sampler_sequence=0` in the final snapshot is a diagnostic-format limitation: the shutdown writer resets that field while preserving the final callback count.
- Milestone 2A acceptance criteria are satisfied.
- This callback remains classified as a safe recurring heartbeat, not the authoritative simulation tick.

## 2026-07-15 — Milestone 2B bridge-caller experiment

Build `0.0.4` sampled the immediate return address observed by the chained heartbeat detour.

Target-device result after menu, world activity, pause-menu, world re-entry, and normal shutdown:

```text
schema=1
state=stopped
minecraft_version=1.26.33.1
sample_stride_callbacks=256
sample_capacity=4096
total_callbacks=312803
samples_reserved=1221
samples_consumed=1221
samples_dropped=0
samples_outside_minecraft_module=1221
call_site_count=0
```

Conclusions:

- The runtime remained stable for `312,803` callbacks.
- All `1,221` telemetry samples were consumed with zero drops.
- Every immediate return address was outside `libminecraftpe.so`.
- The result confirms that the chained Gloss/Preloader bridge, rather than Minecraft's original caller, is visible to the detour.
- Immediate detour return-address capture is therefore rejected as a call-site discovery method.
- Build `0.0.5` replaces it with a read-only static ARM64 scan for direct `B`/`BL` references and stored function-pointer references to the exact heartbeat address.
