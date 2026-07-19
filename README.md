# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2P / version 0.0.23 is a read-only terrain command descriptor census.

This build keeps the validated render and terrain callbacks, adds one additional validated diagnostic callback, and records fixed-size telemetry about recurring terrain command descriptors. It performs no custom geometry submission and uses no overlay renderer.

The diagnostic output is written to:

```text
<mod data directory>/terrain-command-census-status.txt
<mod data directory>/terrain-command-census-timeline.csv
```

All callbacks remain version-locked, reversible, and subject to safe-unload checks.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
