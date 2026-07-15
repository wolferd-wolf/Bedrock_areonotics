# Milestone 2C — Vtable-Neighbour Counter Validation

## Evidence entering this stage

The exact Minecraft Bedrock Android 1.26.33.1 ARM64 binary has:

- GNU build ID `2e318db12824cadb2618754ab7c82fa96fb30659`;
- file size `349243744` bytes;
- heartbeat function offset `0x9d80fac`;
- one vtable-like run from `0x140545a0` to `0x14055350`;
- 438 contiguous executable-function entries;
- heartbeat method at slot 152, byte offset 1216;
- 35 ARM64 virtual-call sequences that load and invoke slot 152.

Those 35 references identify menu-state call paths. They do not identify an authoritative simulation tick.

## Probe selection

Build `0.0.7` probes five non-trivial methods close to the heartbeat entry:

| Slot | Relative to heartbeat | Function offset | Reason |
|---:|---:|---:|---|
| 144 | -8 | `0x9d80558` | Largest nearby implementation before the heartbeat |
| 146 | -6 | `0x9d80bbc` | Medium-size nearby implementation |
| 151 | -1 | `0x9d80eac` | Immediate pre-heartbeat method |
| 153 | +1 | `0x9d8129c` | Immediate post-heartbeat method |
| 160 | +8 | `0x9d82094` | Larger method containing a local virtual call at `0x9d82180` |

## Runtime behavior

Before installing a candidate probe, the module requires all of the following:

1. Exact Minecraft build ID and file size match.
2. The maintained heartbeat signature resolves to `0x9d80fac`.
3. Vtable slot 152 points to the proven heartbeat function.
4. Every selected vtable entry points to its exact recorded executable offset.
5. A readable instruction prefix exists for every candidate.
6. The active preloader exports `GlossHookInternal` and `GlossHookDelete`.

The candidate callbacks are register-preserving internal hooks. They do not call a guessed C++ function signature. Each callback only:

- increments an atomic total;
- classifies the call using the cached result of the proven menu-state method;
- records the first Linux thread ID and counts calls from other threads.

File output runs on a separate sampler thread.

## Output

Summary:

```text
<mod data directory>/vtable-probe-profile.txt
```

Two-second timeline:

```text
<mod data directory>/vtable-probe-timeline.txt
```

The profile uses schema 4 and contains total calls, menu/gameplay splits, calculated rates, thread affinity, and stable-interval minimum/maximum deltas.

## Device test

1. Remove or disable build `0.0.6`.
2. Import and enable build `0.0.7` for Minecraft 1.26.33.1.
3. Remain on the main menu for about 30 seconds.
4. Enter a disposable world and remain idle for about 60 seconds.
5. Walk, jump, place blocks, and break blocks for about 60 seconds.
6. Open the pause menu for about 30 seconds.
7. Resume gameplay for about 30 seconds.
8. Leave the world and remain on the menu for about 30 seconds.
9. Re-enter the world and play for another 30 seconds.
10. Exit Minecraft normally.
11. Provide the complete contents of both probe files.

## Candidate criteria

A promising simulation/update candidate should show most of the following:

- repeatable non-zero call rate during gameplay;
- a stable frequency compatible with an update cadence, such as roughly 20, 30, or 60 calls per second;
- materially lower or stopped activity while the main or pause menu is active;
- strong single-thread affinity;
- stable behavior across world exit and re-entry;
- no crash during normal shutdown or hook removal.

No method will be treated as the simulation tick from frequency alone. A selected method must receive a separate one-method exact-offset validation and longer stability test.

## Failure handling

If Minecraft crashes, do not repeatedly relaunch build `0.0.7`. Disable it, preserve the first tombstone and LeviLauncher log, and confirm build `0.0.6` still works as the stable discovery baseline.
