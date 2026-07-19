# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android mod research project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- Bedrock Behavior Pack, Resource Pack, and stable Script API
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 3D / version 0.0.29 adds the first anchored moving ship reference-frame proxy.

The test flow is:

1. Tap a Ship Core to scan the connected assembly.
2. Tap the same Ship Core again to confirm it.
3. Tap a connected Helm to engage the flight proxy.
4. The detected ship outline rises to a four-meter hover under fixed-step physics.
5. The pilot is held at the moving Helm.
6. Sneak to command a controlled return and landing.

The blue outline is generated from the confirmed assembly's real bounds, exposed faces, Helm positions, and Aero Engine positions. The motion model runs at 20 Hz with gravity compensation, bounded Aero Engine thrust, position/velocity feedback, a mass-based lift-authority check, smooth hover capture, and a controlled return phase.

The native module contains the matching host-tested anchored-flight motion core and exposes its architecture marker in the Android binary. The coordinated content pack supplies the current phone-visible transformed-particle renderer and safe player anchoring bridge.

## Assembly rules

The ship must be separated from terrain before scanning. Any ordinary non-air, non-liquid block touching the ship by a face is considered connected. Break temporary construction supports before tapping the Ship Core.

The assembly requires exactly one connected Ship Core, at least one Helm, and enough Aero Engine thrust for its estimated mass. Safety limits remain 2,048 connected blocks and 64 blocks on each axis.

## Honest implementation boundary

Version 0.0.29 moves a rendered reference-frame proxy and the anchored pilot; it does not remove or translate the original Minecraft blocks. The original structure remains visible at its construction location, and the moving blue outline shows the transform produced by the new physics model.

This milestone deliberately proves whole-assembly transforms, fixed-step lift/hover/return physics, mass/thrust gating, and pilot anchoring before unrestricted movement. The next flight milestone will work toward a movable block mesh and simple terrain collision. Free walking inside the moving ship remains the later 3G milestone.

## Build outputs

GitHub Actions packages:

- `bedrock_aeronautics.levipack` — native ARM64 module;
- `bedrock-aeronautics-content-0.0.29.mcaddon` — blocks, original textures, scripts, assembly particles, and flight-proxy particles;
- the complete diagnostics bundle.

The phone test procedure is documented in `docs/milestone-3d-anchored-flight-proxy-phone-test.md`.

## Asset and legal boundaries

The Aeronautics block textures are original project assets. This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/validate_content_pack.py content
node tests/assembly_preview_scan_test.mjs
node tests/interaction_gate_test.mjs
node tests/flight_proxy_test.mjs
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
