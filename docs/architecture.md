# Architecture

## Target platform

- Minecraft Bedrock Android 1.26.33.1
- ARM64-v8a
- LeviLauncher native-mod packaging
- `LiteLDev/preloader-android` lifecycle SDK
- Single-player first

## Runtime stack

```text
Minecraft Bedrock Android 1.26.33.1
        |
LeviLauncher / native preloader runtime
        |
Bedrock Aeronautics lifecycle module
        |
Bedrock adapter layer
        |
Aeronautics core
  |- ship-local storage
  |- transforms and math
  |- rigid-body physics
  |- collision acceleration
  |- machinery graph
  `- serialization
```

The core must remain testable without Minecraft. Bedrock symbols, offsets, hooks, rendering calls, world access, player integration, and native interaction stay inside `src/bedrock/` when those systems are introduced.

## Loader contract

Android modules are packaged as a LeviLauncher `preload-native` mod. The package contains a stable mod directory, `manifest.json`, and `libbedrock_aeronautics.so`.

The native library exports `PLGetModRegistration` through `PL_REGISTER_MOD`. The registered object implements the supported lifecycle phases:

- `load()`
- `enable()`
- `disable()`
- `unload()`

The earlier placeholder `mod_init()` convention was removed before device testing because it was not the supported LeviLauncher lifecycle ABI.

## Dependency boundary

`preloader-android` is the Android runtime dependency. Project Amethyst remains reference material, but post-1.21 development is a bring-your-own-types workflow. Bedrock Aeronautics therefore owns its 1.26.33.1 signatures, layouts, validation rules, and adapter code rather than assuming 1.21 definitions remain valid.

## Core rules

1. One rigid body per assembled ship.
2. Blocks are stored in ship-local coordinates.
3. Ships use section-based storage rather than one entity per block.
4. Rendering applies a ship transform to section geometry.
5. Collision queries are transformed into ship-local space.
6. Physics uses a fixed timestep.
7. Hook installation validates the exact supported binary before patching.
8. Unsupported blocks fail assembly safely.
9. Native crashes must be diagnosable from retained symbols and build metadata.
10. Loader lifecycle code must remain separate from Bedrock hooks.

## Current Milestone 3E module

The native module contains:

- exact-binary-validated heartbeat, ClientLevel tick, terrain-task, terrain-helper, and render hooks;
- a fixed 20 Hz physics scheduler with coherent render snapshots;
- tested ship assembly, autopilot handoff, and anchored flight-proxy cores;
- a read-only helper output-ownership census;
- safe lifecycle installation, restoration, and retained telemetry.

The previous payload gate observed 180,288 helper calls without one captured destination or one container append. The container remained exactly one 64-byte element. Milestone 3E treats that result as a failed assumption, not permission to write.

For each valid helper call, the new probe compares the existing command element before and after the original Minecraft helper. For each descriptor it also claims at most one 128-byte destination sample, copying it through `process_vm_readv` against the current process so an invalid or short mapping produces a telemetry failure instead of a direct faulting dereference. All storage remains fixed-size and callback-safe.

Native command insertion stays disabled. A later submission patch requires a stable changed-qword layout, proven object ownership, exact fingerprints, a sustained phone run, and successful hook restoration. Return-value capture remains deferred until disassembly proves the AArch64 ABI.

The coordinated content pack retains the version 0.0.29 anchored particle-flight bridge at version 0.0.30 for phone regression tests. Native arbitrary block-mesh submission and terrain collision remain outside the active runtime path.

## Compatibility boundary

The active profile is permanently version-locked to Minecraft Bedrock Android 1.26.33.1 until a deliberate port is started. Offsets and signatures from 1.21 or another 1.26 build must never be silently mixed into this profile.
