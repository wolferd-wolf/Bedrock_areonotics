# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2C — counter-only vtable-neighbour validation.

Milestone 2A proved the native detour chain with 375,298 stable callback invocations, world exit/re-entry, and clean shutdown. Subsequent discovery builds established that:

- the proven heartbeat is virtual method slot 152 in a 438-entry executable-pointer table;
- the table starts at module-relative offset `0x140545a0`;
- the heartbeat target is `0x9d80fac`;
- 35 ARM64 virtual-call sequences invoke slot 152;
- those sequences are callers of the menu-state method and are not automatically simulation ticks.

Build 0.0.7 performs the first dedicated counter-only candidate validation. It installs register-preserving probes on five non-trivial neighbouring virtual methods:

- slot 144 — `0x9d80558`;
- slot 146 — `0x9d80bbc`;
- slot 151 — `0x9d80eac`;
- slot 153 — `0x9d8129c`;
- slot 160 — `0x9d82094`.

Each probe only increments atomic counters, records cached menu-state classification, and records thread affinity. A separate sampler thread writes:

```text
<mod data directory>/vtable-probe-profile.txt
<mod data directory>/vtable-probe-timeline.txt
```

The probes do not inspect or mutate world, entity, block, player, render, or input state. A method will only become a dedicated tick candidate if its runtime frequency, menu/gameplay split, thread affinity, stability, and lifecycle behavior support that conclusion.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
