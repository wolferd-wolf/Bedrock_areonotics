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

Milestone 3A / version 0.0.25 establishes the custom-block and single-player autopilot foundation.

This milestone adds:

- the custom `aeronautics:ship_core`, `aeronautics:helm`, and `aeronautics:aero_engine` blocks;
- an importable `.mcaddon` containing coordinated behavior and resource packs;
- a deterministic C++ manual-to-autopilot handoff controller;
- heading, altitude, and forward-speed hold;
- emergency braking;
- bounded control outputs and clamped integrators;
- host tests for handoff, heading wraparound, correction direction, emergency braking, and output limits.

When a pilot eventually leaves a live Helm, the controller captures the current heading, altitude, and forward speed, blends away from the last manual input for one second, and enters autopilot hold. A decorative bot pilot may be added later, but flight authority remains deterministic native code.

## Honest implementation boundary

Version 0.0.25 does not yet assemble or move a Minecraft structure. The blocks are construction and interaction anchors, and the autopilot controller is compiled and tested but not connected to a live ship.

The existing read-only RenderDragon terrain diagnostics remain in place while moving-ship rendering is developed. Geometry submission, native block-event integration, ship scanning, moving collision, and walkable reference frames remain later milestones.

## Build outputs

GitHub Actions packages:

- `bedrock_aeronautics.levipack` — native ARM64 module;
- `bedrock-aeronautics-content-0.0.25.mcaddon` — custom blocks;
- the complete diagnostics bundle.

The phone test procedure is documented in `docs/milestone-3a-phone-test.md`.

## Safety and legal boundaries

This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source. The content pack uses texture aliases resolved from the player's installed licensed game; it does not copy Mojang texture files.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/validate_content_pack.py content
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
