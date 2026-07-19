# Bedrock Aeronautics

Version-locked native Minecraft Bedrock Android aeronautics project.

## Target

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher-compatible native module
- Bedrock Behavior Pack, Resource Pack, and stable Script API
- C++20
- GitHub Actions cloud builds

## Current milestone

Milestone 3E / version 0.0.30 is the native render-submission gate.

The 0.0.29 phone telemetry proved that the terrain helper runs reliably, but it also disproved the earlier payload assumption: all 180,288 helper observations missed the supposed destination, while the suspected command container stayed fixed at 64 used bytes and 64 capacity bytes. Injecting geometry there would be unsafe.

Version 0.0.30 therefore remains read-only and measures the two plausible Minecraft-owned outputs:

1. the existing 64-byte command element before and after every helper call, so in-place mutation is visible;
2. one bounded 128-byte destination-object snapshot per helper descriptor, read with a fault-safe self-process copy.

The probe records exact changed-qword masks, first/last command values, destination before/after values, thread ownership, helper ordinals, fingerprints, and lifecycle teardown state. It does not fabricate a helper return ABI and it does not submit geometry until one ownership path is proven.

## Existing phone-visible gameplay

The coordinated 0.0.30 add-on retains the working anchored flight proxy:

1. Tap a Ship Core to scan the connected assembly.
2. Tap the Ship Core again to confirm it.
3. Tap a connected Helm to lift the blue assembly outline.
4. The pilot remains anchored at the moving Helm.
5. Sneak to return and land.

This content bridge is kept for regression testing. It is not the final renderer or physics integration.

## Honest implementation boundary

Version 0.0.30 does not yet render a native moving block mesh. No visible native cube is expected. The original Minecraft blocks remain stationary, and the add-on's blue particle outline remains the only phone-visible moving proxy.

The next native version will enable a single diagnostic geometry submission only if the 0.0.30 telemetry proves a stable Minecraft-owned destination and lifecycle. Terrain collision and free walking inside the moving reference frame follow after native mesh submission is stable.

## Build outputs

GitHub Actions packages:

- `bedrock_aeronautics.levipack` — version 0.0.30 native ARM64 ownership probe;
- `bedrock-aeronautics-content-0.0.30.mcaddon` — coordinated blocks and the existing anchored-flight test bridge;
- the complete diagnostics bundle.

Use the procedure in `docs/milestone-3e-native-output-ownership-phone-test.md`.

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
