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

## 2026-07-15 — Milestone 2B.2 indirect dispatch scan

Build `0.0.6` verified the pointer table and scanned for ARM64 virtual calls using the heartbeat slot.

Key target-device result:

```text
schema=3
state=stopped
minecraft_version=1.26.33.1
module_build_id=2e318db12824cadb2618754ab7c82fa96fb30659
module_file_size=349243744
discovery_method=arm64_indirect_dispatch_scan
heartbeat_target_offset=0x9d80fac
total_callbacks=36028
scan_state=complete
scan_duration_ms=1629
direct_branch_reference_count=0
pointer_reference_count=1
dispatch_table_candidate_count=1
indirect_call_reference_count=35
pointer_reference.0.offset=0x14054a60
dispatch_table.0.run_start_offset=0x140545a0
dispatch_table.0.run_end_offset=0x14055350
dispatch_table.0.entry_count=438
dispatch_table.0.slot_index=152
dispatch_table.0.slot_offset_bytes=1216
dispatch_table.0.vtable_like=true
```

The eight neighbouring function offsets on each side of the heartbeat are:

```text
-8  0x9d80558
-7  0x9d80b84
-6  0x9d80bbc
-5  0x9d80dc0
-4  0x9d80de4
-3  0x9d80e10
-2  0x9d80e58
-1  0x9d80eac
 0  0x9d80fac
+1  0x9d8129c
+2  0x9d815a8
+3  0x9d815b8
+4  0x9d815c8
+5  0x9d815d8
+6  0x9d815e8
+7  0x9d815f8
+8  0x9d82094
```

Conclusions:

- The stored pointer is inside a real vtable-like run of 438 executable methods.
- The heartbeat is virtual method slot 152.
- All 17 inspected neighbouring entries are executable Minecraft functions.
- Thirty-five code locations contain narrow `LDR` plus `BLR`/`BR` virtual-call patterns for slot 152.
- Those 35 locations are call paths for the known menu-state method, not independent simulation-tick candidates.
- Build `0.0.7` moves to runtime comparison of five selected neighbouring methods using register-preserving, counter-only probes and no gameplay-state access.
