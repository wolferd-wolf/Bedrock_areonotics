# Milestone 3B phone test — original textures and assembly foundation

Target: Minecraft Bedrock Android 1.26.33.1 with LeviLauncher.

## Install

1. Back up the test world.
2. Replace the previous native module with the version 0.0.26 LeviPack.
3. Open the version 0.0.26 `.mcaddon` with Minecraft.
4. If Minecraft keeps the old pack cached, remove the old Bedrock Aeronautics behavior and resource packs, then import 0.0.26 again.
5. Create a new Creative test world and activate both Bedrock Aeronautics packs.

## Visual test

Run:

```mcfunction
/give @s aeronautics:ship_core
/give @s aeronautics:helm
/give @s aeronautics:aero_engine
```

Confirm:

- Ship Core is dark gunmetal with cyan energy details, not an iron block.
- Helm is navy with amber instrument details, not wooden planks.
- Aero Engine is graphite with orange heat/vent details, not a copper block.
- Inventory icons and placed blocks use the same custom textures.
- Blocks survive save, exit, and world reload.

## Assembly preparation test

Build a small connected shape containing one Ship Core, at least one Helm, and at least one Aero Engine. This version compiles and host-tests the native six-face assembly builder, mass/center-of-mass/inertia calculation, deterministic block ordering, and safety bounds.

## Honest boundary

Version 0.0.26 does not yet invoke assembly from an in-game block interaction and does not move the structure. The next integration step is to bind Ship Core activation to a Bedrock world block snapshot, then hand the validated assembly snapshot to the moving physics/render frame.
