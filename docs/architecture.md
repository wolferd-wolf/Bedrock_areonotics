# Architecture

## Target platform

- Minecraft Bedrock Android 1.21.0.03
- ARM64-v8a
- LeviLauncher native-mod packaging
- `LiteLDev/preloader-android` lifecycle SDK
- Single-player first

## Runtime stack

```text
Minecraft Bedrock Android 1.21.0.03
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

`preloader-android` is the Android runtime dependency. Project Amethyst remains a useful 1.21.0.3 reverse-engineering and type-layout reference, but its current build setup is Windows-oriented and is not treated as the Android loader.

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

## Current loader-proof module

The module currently performs no Minecraft hooks. Its lifecycle only:

- creates its private data directory;
- logs the project version and exact target;
- logs enable, disable, and unload transitions.

This deliberately limits Milestone 1 to proving that the package can be imported and loaded without destabilizing Minecraft.

## Compatibility boundary

The repository is permanently version-locked until a deliberate port is started. Offsets and signatures for another Minecraft release must never be silently mixed into the 1.21.0.03 profile.
