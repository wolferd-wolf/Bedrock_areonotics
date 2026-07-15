# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2B.2 — indirect ARM64 dispatch discovery.

Milestone 2A proved the native detour chain with 375,298 stable callback invocations, world exit/re-entry, and clean shutdown. Later diagnostics established that:

- the immediate detour caller belongs to the chained Gloss/Preloader bridge;
- no direct AArch64 `B` or `BL` instruction targets the heartbeat function;
- exactly one stored function pointer references the heartbeat, at module-relative offset `0x14054a60`.

Build 0.0.6 treats indirect dispatch as the leading hypothesis. It performs a read-only analysis that:

- finds the contiguous run of executable function pointers surrounding the heartbeat entry;
- derives the candidate table slot index and byte offset;
- records neighbouring entry classifications without persisting absolute process addresses;
- scans executable Minecraft mappings for `LDR` plus `BLR`/`BR` sequences that load and invoke that exact slot.

The diagnostic output is written to:

```text
<mod data directory>/tick-discovery-profile.txt
```

The scan does not read or mutate world, entity, render, block, player, or input state. Any candidate call sites remain discovery leads and require a separate counter-only runtime hook before being treated as an update/tick path.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
