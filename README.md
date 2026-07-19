# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- Bedrock Behavior Pack and Resource Pack
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 3B / version 0.0.26 establishes original block visuals and deterministic ship assembly data.

This milestone adds:

- original 64x64 textures for `aeronautics:ship_core`, `aeronautics:helm`, and `aeronautics:aero_engine`;
- a core-seeded six-face flood fill that excludes disconnected blocks;
- deterministic local block ordering;
- total mass, local center of mass, diagonal inertia, and local bounds;
- required Helm and Aero Engine validation;
- limits of 2048 connected blocks and 64 blocks on each axis;
- host tests for connectivity, deterministic results, mass properties, requirements, duplicate positions, and safety bounds.

The version 0.0.25 solo-pilot autopilot remains compiled: leaving the Helm captures heading, altitude, and forward speed, blends for one second, and enters native autopilot hold.

## Honest implementation boundary

Version 0.0.26 provides the data structure that a moving ship needs, but does not yet activate it from an in-game Ship Core. The blocks remain stationary Minecraft blocks in this build.

The next integration milestone must capture a bounded Bedrock world snapshot when the player activates the Ship Core, translate it into the tested assembly snapshot, then hand that snapshot to the moving physics/render reference frame. Player-relative moving collision and walking inside a moving ship remain later integration work.

The existing read-only RenderDragon terrain diagnostics remain in place while moving-ship rendering is developed.

## Build outputs

GitHub Actions packages:

- `bedrock_aeronautics.levipack` — native ARM64 module;
- `bedrock-aeronautics-content-0.0.26.mcaddon` — custom blocks and original textures;
- the complete diagnostics bundle.

The phone test procedure is documented in `docs/milestone-3b-phone-test.md`.

## Asset and legal boundaries

The three Aeronautics block textures are original project assets. This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/validate_content_pack.py content
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
