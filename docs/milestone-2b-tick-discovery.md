# Milestone 2B — Safe Tick Discovery Telemetry

## Purpose

Milestone 2A proved that the native hook chain is stable on Minecraft Bedrock Android 1.26.33.1. It did not prove that the selected read-only menu-state callback is the simulation tick.

Milestone 2B therefore gathers evidence about the Minecraft call sites that invoke the proven callback before attempting any new private-function hook.

## Runtime behavior

The existing heartbeat detour still:

- calls the original callback;
- preserves and returns the original result;
- avoids world, player, entity, block, render, and input state;
- performs no file I/O from the hot callback.

Every 256th callback, the detour reserves one fixed-capacity telemetry slot and records:

- the caller return address;
- the Linux thread ID;
- whether the original callback reported that a menu was showing.

A separate sampler thread consumes those slots and writes:

```text
<mod data directory>/tick-discovery-profile.txt
```

The fixed buffer stores 4,096 samples, representing up to roughly 1,048,576 callbacks at the configured sampling stride. Further samples are counted as dropped rather than allocating memory or blocking the callback.

## Profile fields

The profile includes:

- total callback count;
- samples reserved, consumed, and dropped;
- samples whose return address was outside `libminecraftpe.so`;
- unique Minecraft call-site offsets relative to the module load base;
- per-call-site sample counts;
- menu-true and menu-false counts;
- per-call-site thread IDs and counts.

Absolute process addresses are deliberately not persisted. Relative offsets remain useful across runs of the exact same binary despite ASLR.

## Test sequence

1. Start Minecraft and remain on the main menu for about one minute.
2. Enter the test world and remain idle for about two minutes.
3. Walk, jump, place, and break blocks for about two minutes.
4. Open the pause menu for about one minute.
5. Return to gameplay for about one minute.
6. Leave the world, re-enter it, and play for another minute.
7. Exit Minecraft normally.
8. Read both `heartbeat-status.txt` and `tick-discovery-profile.txt`.

## Acceptance criteria

- Minecraft remains stable for the full test.
- The heartbeat count continues increasing.
- `tick-discovery-profile.txt` is created and ends with `state=stopped`.
- `samples_consumed` is greater than zero.
- At least one call-site offset or a clear outside-module result is recorded.
- Clean shutdown removes the hook without a crash.

## Decision after the run

A call site is only a discovery lead, not automatically a tick function. The next stage will compare call-site frequency, menu-state distribution, and thread affinity. A dedicated candidate will then receive its own exact-version signature, executable-range validation, instruction-prefix fingerprint, and counter-only detour before any game state is touched.
