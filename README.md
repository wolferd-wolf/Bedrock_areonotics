# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2C — counter-only update-candidate validation.

Milestone 2A proved the native detour chain with 375,298 stable callback invocations, world exit/re-entry, and clean shutdown. Subsequent discovery established that:

- the proven heartbeat is virtual method slot 152 in a 438-entry executable-pointer table;
- the table starts at module-relative offset `0x140545a0`;
- the heartbeat target is `0x9d80fac`;
- 35 ARM64 virtual-call sequences invoke slot 152;
- those sequences are callers of the menu-state method and are not automatically simulation ticks.

Builds 0.0.7 and 0.0.8 attempted register-preserving inline probes on five neighbouring methods. The 4-byte hooks were rejected, while the full-size hooks caused Minecraft to exit during startup on the target device. Inline candidate hooks are therefore abandoned for this stage.

Build 0.0.9 uses one delayed data-pointer probe instead:

- candidate: vtable slot 160;
- slot address offset: `0x14054aa0`;
- original target offset: `0x9d82094`;
- activation waits at least eight seconds and requires a working menu observer;
- only the aligned 64-bit vtable pointer is temporarily replaced;
- no Minecraft executable code bytes are modified;
- an ARM64 trampoline preserves incoming argument, floating-point, condition-code, and link-register state before counting and tail-branching to the original method;
- the original pointer and page protection are restored during disable or unload;
- unload is refused if restoration cannot be proven.

The diagnostic output is written to:

```text
<mod data directory>/vtable-probe-profile.txt
<mod data directory>/vtable-probe-timeline.txt
```

The probe does not inspect or mutate world, entity, block, player, render, or input state. Slot 160 remains only a candidate until its runtime frequency, menu/gameplay split, thread affinity, lifecycle behavior, and stability are measured on the exact target build.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
