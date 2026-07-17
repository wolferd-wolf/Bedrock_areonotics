# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 2O / version 0.0.22 is a read-only RenderDragon diagnostic build.

The preceding phone test proved that:

- the exact `LevelRendererCamera::render` hook is stable across thousands of frames;
- the camera object pointer chain is valid;
- the 192-byte region at camera object offset `0x158` is six frustum plane equations followed by eight frustum corner vectors, not three 4x4 matrices;
- the former `_insertChunkLayer` task target was inactive in the tested render path.

Version 0.0.22 therefore:

- validates the exact Minecraft binary fingerprint and both hook instruction prefixes;
- derives camera forward direction, near/far distances, horizontal/vertical field of view, and aspect ratio from the native frustum;
- records targeted `ViewRenderObject` vectors to identify the absolute camera position source;
- hooks the narrower `framebuilderInsertTerrainCommandsForChunks` task operator and records its closure fields and render-block flag;
- remains read-only and submits no custom geometry;
- restores every hook before unload and refuses unsafe unload.

The diagnostic output is written to:

```text
<mod data directory>/frustum-terrain-discovery-status.txt
<mod data directory>/frustum-terrain-discovery-timeline.csv
```

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. Only original project code, original assets, build scripts, tests, documentation, compatibility fingerprints, and hook metadata belong here.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
