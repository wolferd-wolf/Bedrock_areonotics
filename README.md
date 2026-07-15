# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2B.1 — static ARM64 call-site discovery.

Milestone 2A proved the native detour chain with 375,298 stable callback invocations, world exit/re-entry, and clean shutdown. The first Milestone 2B experiment then proved that immediate detour return addresses resolve to the chained Gloss/Preloader bridge rather than Minecraft's original caller.

Build 0.0.5 replaces that failed method with a read-only scan of the exact loaded `libminecraftpe.so` mappings for:

- direct AArch64 `B` and `BL` instructions targeting the proven heartbeat function;
- stored function-pointer values equal to the exact heartbeat address.

The diagnostic output is written to:

```text
<mod data directory>/tick-discovery-profile.txt
```

The scan does not read or mutate world, entity, render, block, player, or input state. Any references found are discovery leads only and must receive separate runtime validation before being treated as an update/tick path.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
