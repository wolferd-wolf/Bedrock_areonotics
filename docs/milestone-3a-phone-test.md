# Milestone 3A phone test — custom blocks and autopilot foundation

## Build

- Native version: 0.0.25
- Target: Minecraft Bedrock Android 1.26.33.1 ARM64
- Native package: `bedrock-aeronautics-0.0.25-autopilot-foundation.levipack`
- Content package: `bedrock-aeronautics-content-0.0.25.mcaddon`

## Installation order

1. Back up the test world.
2. Replace the previous native module with the 0.0.25 LeviPack in LeviLauncher.
3. Open the 0.0.25 `.mcaddon` with Minecraft and wait for both packs to import.
4. Create a new Creative test world.
5. Activate **Bedrock Aeronautics Behavior** and its required resource pack.
6. Launch the world through LeviLauncher.

## Block test

In Creative inventory or with commands, obtain:

- `aeronautics:ship_core`
- `aeronautics:helm`
- `aeronautics:aero_engine`

Place, break, and craft each block. Verify:

- all three names appear correctly;
- each block renders on every face;
- held-item visuals render;
- selection and collision are full-block;
- the Ship Core and Aero Engine emit their configured low light level;
- saving and reopening the world preserves the blocks.

Commands may be used if Creative search does not index the new blocks:

```text
/give @s aeronautics:ship_core
/give @s aeronautics:helm
/give @s aeronautics:aero_engine
```

## Native smoke test

Stay in the world for at least two minutes, move across several chunks, leave the world, and close Minecraft normally. Confirm the existing native diagnostic status files still update and report `failure_reason=none`.

## Expected boundary

This build does not assemble or move the blocks. The autopilot is a compiled and host-tested native controller awaiting connection to a live Ship Core, ship state, renderer, and physics body.
