# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2B — safe tick discovery telemetry.

Milestone 2A proved the native detour chain with 375,298 stable callback invocations, world exit/re-entry, and clean shutdown. Milestone 2B samples the proven heartbeat callback's Minecraft call sites, thread IDs, and menu-state result without reading or mutating world, entity, render, block, player, or input state.

The diagnostic output is written to:

```text
<mod data directory>/tick-discovery-profile.txt
```

The profile will be used to select and validate a dedicated update/tick candidate for the exact 1.26.33.1 binary before any gameplay state is accessed.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
