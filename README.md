# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2Q / version 0.0.24 is a read-only terrain-command payload decoder.

The 0.0.23 phone census proved that:

- the active terrain task and helper hooks are stable;
- 6,917 terrain tasks produced 110,672 helper calls;
- every observed task made exactly 16 helper calls;
- all helper vector, context, render-object, and view arguments matched their task closure;
- the command vector uses 64-byte elements;
- 16 stable descriptor and mode ordinals exist;
- terrain construction runs on worker threads rather than the Minecraft render thread.

Version 0.0.24 therefore:

- preserves the exact Minecraft 1.26.33.1 binary fingerprint and instruction-prefix gates;
- observes the command vector before and after Minecraft's original terrain helper;
- accepts payload memory only when the full 64-byte range is proven inside the vector;
- records eight first and last 64-bit fields for every descriptor;
- reports changed, varying, non-zero, module-pointer, vector-pointer, and aligned-pointer-like field masks;
- correlates the helper destination with the post-call vector element and records exact 64-byte growth;
- uses fixed atomic storage with no heap allocation, locks, or file I/O in the hooked callback;
- remains discovery-only and submits no custom geometry.

The diagnostic output is written to:

```text
<mod data directory>/terrain-command-payload-status.txt
<mod data directory>/terrain-command-payload-timeline.csv
```

The phone test procedure is documented in `docs/milestone-2q-phone-test.md`.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
