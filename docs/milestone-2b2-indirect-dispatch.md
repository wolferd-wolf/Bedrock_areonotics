# Milestone 2B.2 — Indirect ARM64 Dispatch Discovery

## Evidence entering this stage

The exact Minecraft 1.26.33.1 ARM64 binary has:

- heartbeat function offset `0x9d80fac`;
- GNU build ID `2e318db12824cadb2618754ab7c82fa96fb30659`;
- no direct `B` or `BL` reference to the heartbeat;
- one stored pointer equal to the heartbeat address at offset `0x14054a60`.

This makes indirect dispatch the leading explanation.

## Runtime behavior

Build `0.0.6` keeps the accepted heartbeat detour unchanged. A separate sampler thread performs one read-only scan.

For each stored heartbeat pointer, the scanner:

1. finds the readable non-executable mapping containing the pointer;
2. walks backward and forward while adjacent entries point into executable Minecraft mappings;
3. records the resulting table run, entry count, heartbeat slot index, and slot byte offset;
4. records eight neighbouring entries on each side and classifies each value;
5. scans executable mappings for the narrow virtual-dispatch sequence:
   - `LDR Xvtable, [Xobject]`;
   - `LDR Xfunction, [Xvtable, #slot]`;
   - `BLR Xfunction` or `BR Xfunction`.

The second and third instructions may be separated by up to four instructions, and the vtable load may appear up to four instructions before the slot load.

## Safety boundary

The scanner:

- reads only mappings already attributed to the exact `libminecraftpe.so` binary;
- performs no writes or patches;
- performs no file I/O inside the heartbeat callback;
- accesses no world, player, entity, block, render, or input objects;
- records module-relative offsets rather than absolute process addresses.

A discovered indirect call site is not automatically a game tick. It must pass a separate exact-offset executable-range check, instruction-prefix fingerprint, counter-only detour, stability test, and lifecycle test.

## Device test

1. Replace build `0.0.5` with build `0.0.6`.
2. Launch Minecraft 1.26.33.1.
3. Enter a world and move normally.
4. Keep Minecraft open until `tick-discovery-profile.txt` reports `scan_state=complete`.
5. Exit normally.
6. Provide the complete `tick-discovery-profile.txt`.

## Important output fields

```text
schema=3
discovery_method=arm64_indirect_dispatch_scan
dispatch_table_candidate_count=
indirect_call_reference_count=
```

For each table candidate, the report includes:

```text
dispatch_table.N.entry_count=
dispatch_table.N.slot_index=
dispatch_table.N.slot_offset_bytes=
dispatch_table.N.vtable_like=
```

For each matched call sequence, it includes:

```text
indirect_call.N.offset=
indirect_call.N.slot_offset_bytes=
indirect_call.N.object_register=
indirect_call.N.vtable_register=
indirect_call.N.function_register=
indirect_call.N.branch_kind=
```
