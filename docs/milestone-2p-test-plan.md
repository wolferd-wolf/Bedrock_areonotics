# Milestone 2P — Terrain Command Descriptor Census

Version 0.0.23 is a read-only diagnostic build for Minecraft Bedrock Android 1.26.33.1 ARM64.

## Purpose

The build records calls to the verified terrain command construction helper at module offset `0x1266ee9c`, but only while execution is inside the verified outer terrain task at `0x0bdb87b8`.

It does not submit custom geometry.

## Phone test

1. Install the 0.0.23 LeviPack after disabling the previous version.
2. Enter a normal overworld area with opaque terrain, vegetation, glass, and water visible when possible.
3. Stand still for 10 seconds.
4. Slowly rotate through 360 degrees.
5. Walk forward for 15 seconds.
6. Look through glass and across water for 15 seconds.
7. Pause and resume once.
8. Save and exit normally.

Upload:

- `terrain-command-census-status.txt`
- `terrain-command-census-timeline.csv`

## Expected safety fields

```text
fingerprint_validated=true
outer_prefix_validated=true
helper_prefix_validated=true
callbacks_in_flight=0
hook_restore_succeeded=true
safe_to_unload=true
failure_reason=none
```

## Expected discovery fields

```text
outer_terrain_task_calls > 0
helper_total_calls > 0
helper_scoped_calls > 0
helper_unscoped_calls may be nonzero
census_slot_count > 0
```

Each census slot records the descriptor relative address, mode, ordinal, bitmask, copied-container used and capacity byte counts, view-pointer agreement, thread identity, and call count.
