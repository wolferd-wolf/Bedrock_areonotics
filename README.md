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

Milestone 3C / version 0.0.27 adds the first live in-game ship assembly preview.

Tap a Ship Core to:

- scan all face-connected, non-air, non-liquid blocks;
- exclude disconnected blocks;
- require exactly one connected Ship Core, one or more Helms, and one or more Aero Engines;
- calculate block count, dimensions, estimated mass, and local center of mass;
- mark exposed connected faces with cyan particles;
- draw a cyan particle boundary around a valid ship;
- highlight Helms in amber and Aero Engines in orange;
- draw invalid assemblies in red with a specific reason;
- show a phone-friendly title, action-bar summary, and chat report.

Tap the same core again to confirm. Sneak-tap to cancel. Previews expire after 12 seconds or when the player moves more than 24 blocks from the core. Confirmation saves the core location and compact assembly summary as a world dynamic property.

The native version 0.0.26 assembly builder and version 0.0.25 solo-pilot autopilot remain compiled and tested.

## Assembly rules

The ship must be separated from terrain before scanning. Any ordinary non-air, non-liquid block touching the ship by a face is considered connected. Break temporary construction supports before tapping the Ship Core.

Safety limits remain 2,048 connected blocks and 64 blocks on each axis. These limits prevent an accidental scan from walking through the ground or a large terrain structure.

## Honest implementation boundary

Version 0.0.27 performs a real live scan and visible preview, but does not remove blocks from the Minecraft grid or move the structure.

The outline uses Bedrock client particles for a reliable phone-testable integration. The project’s native RenderDragon diagnostics still do not submit arbitrary line geometry. The next milestone will consume the confirmed snapshot and create the first movable render/physics proxy. Walking inside the moving reference frame remains later work.

## Build outputs

GitHub Actions packages:

- `bedrock_aeronautics.levipack` — native ARM64 module;
- `bedrock-aeronautics-content-0.0.27.mcaddon` — blocks, original textures, scripts, and preview particles;
- the complete diagnostics bundle.

The phone test procedure is documented in `docs/milestone-3c-phone-test.md`.

## Asset and legal boundaries

The Aeronautics block textures are original project assets. This repository must not contain Minecraft APKs, `libminecraftpe.so`, proprietary Mojang assets, access tokens, signing keys, or decompiled proprietary source.

## Build locally

```bash
cmake -S . -B build -G Ninja -DBEDROCK_AERONAUTICS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/validate_content_pack.py content
node tests/assembly_preview_scan_test.mjs
```

Android builds are produced by GitHub Actions and uploaded as workflow artifacts.
