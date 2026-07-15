# Milestone 2A — Safe Recurring Callback Proof

## Purpose

Before attempting a private Minecraft game-tick function, prove that the native hook chain works safely on the target Android build.

This stage hooks the read-only `isShowingMenu` callback that LeviLauncher itself already resolves and hooks. The signature is taken from LeviLaunchroid's maintained preloader rules for Minecraft `1.26.30.00` and newer.

This callback is expected to run repeatedly, but it is **not yet treated as the authoritative simulation tick**.

## Safety gates

The module refuses to install the heartbeat detour unless all of the following pass:

1. `libminecraftpe.so` is present in `/proc/self/maps`.
2. The exact `1.26.33.1` compatibility profile resolves the maintained signature.
3. The resolved address is four-byte aligned for AArch64.
4. The resolved address lies inside a readable and executable mapping belonging to `libminecraftpe.so`.
5. The first 32 bytes at the target can be read safely.
6. The preloader hook API returns a valid original-function trampoline.

Failure leaves the mod enabled in compatibility-safe mode without a hook.

## Runtime behavior

The detour:

- calls the original callback;
- preserves and returns its result;
- increments one atomic counter;
- does not access world, player, entity, block, render, or input state.

Logging is performed by a separate sampler thread every ten seconds so the hot callback does not write logs directly.

Expected messages:

```text
Compatibility profile accepted: mc=1.26.33.1, build_id=..., module_size=..., heartbeat_offset=...
Read-only heartbeat hook installed; no world or render state is modified
Heartbeat sample: total_callbacks=..., callbacks_last_10s=...
```

## Compatibility report

At runtime the module writes:

```text
<mod data directory>/compatibility-report.txt
```

The report records:

- Minecraft version profile;
- resolved `libminecraftpe.so` path;
- module file size;
- GNU ELF build ID when available;
- load base;
- callback relative address;
- first 32 instruction bytes;
- signature source.

This evidence will become the exact binary fingerprint for later hooks.

## Pass criteria

- Package imports and enables.
- Compatibility profile is accepted.
- Heartbeat callback count increases in the menu and/or a world.
- Minecraft remains stable for at least ten minutes.
- Leaving and re-entering a world does not crash.
- Disabling or exiting removes the hook cleanly.
- The compatibility report is created.

## Next stage

After this callback proof passes, Milestone 2B will identify a dedicated `1.26.33.1` update/tick function, validate it against the captured binary fingerprint, and repeat the same no-state-mutation counter test.
