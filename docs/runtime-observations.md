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

## 2026-07-15 — Milestone 2B.1 static reference scan

Build `0.0.5` scanned the exact loaded Minecraft mappings for direct AArch64 `B`/`BL` instructions and stored function pointers targeting the proven heartbeat function.

Target-device result:

```text
schema=2
state=stopped
minecraft_version=1.26.33.1
module_build_id=2e318db12824cadb2618754ab7c82fa96fb30659
module_file_size=349243744
discovery_method=arm64_static_reference_scan
heartbeat_target_offset=0x9d80fac
total_callbacks=215613
scan_state=complete
scan_duration_ms=768
executable_bytes_scanned=333897728
readable_data_bytes_scanned=12648448
direct_branch_reference_count=0
pointer_reference_count=1
pointer_reference.0.offset=0x14054a60
```

Conclusions:

- The complete static scan finished in `768 ms` while the heartbeat remained stable for `215,613` callbacks.
- No direct branch targets the heartbeat function.
- Exactly one stored function pointer references it, at module-relative offset `0x14054a60`.
- The evidence indicates indirect dispatch, with a vtable or similar function-pointer table as the leading hypothesis.
- Build `0.0.6` will identify the surrounding executable-pointer run, derive the candidate slot index, capture neighbouring entry classifications, and scan for ARM64 `LDR` plus `BLR`/`BR` sequences using that exact slot.
