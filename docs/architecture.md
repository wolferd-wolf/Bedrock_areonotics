# Architecture

## Target platform

- Minecraft Bedrock Android 1.21.0.03
- ARM64-v8a
- Native C++ module loaded through LeviLauncher-compatible infrastructure
- Single-player first

## Layering

```text
Minecraft Bedrock 1.21.0.03
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

## Current bootstrap module

The initial module exports:

- `mod_init()`
- `bedrock_aeronautics_version()`

At this stage, `mod_init()` only emits a diagnostic log. No Minecraft functions are hooked yet.

## Compatibility boundary

The repository is permanently version-locked until a deliberate port is started. Offsets and signatures for another Minecraft release must never be silently mixed into the 1.21.0.03 profile.
