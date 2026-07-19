# Milestone 3C phone test — live assembly preview

Target: Minecraft Bedrock Android 1.26.33.1 with LeviLauncher.

## Install

1. Back up the test world.
2. Replace the previous native module with the version 0.0.27 LeviPack.
3. Remove the older Bedrock Aeronautics behavior and resource packs from Minecraft storage.
4. Open the version 0.0.27 `.mcaddon` with Minecraft.
5. Create a new Creative test world and activate both Bedrock Aeronautics packs.
6. Experimental toggles are not required.

## Build a test ship

Obtain the three special blocks:

```mcfunction
/give @s aeronautics:ship_core
/give @s aeronautics:helm
/give @s aeronautics:aero_engine
```

Build a small connected structure with exactly one Ship Core, at least one Helm, at least one Aero Engine, and a few ordinary solid blocks.

The structure must be separated from the ground and other terrain before scanning. A solid support still touching the ship is considered connected. Build with temporary supports, then break the final support before tapping the core.

## Preview controls

- Tap the Ship Core once: scan and start the 12-second preview.
- Tap the same Ship Core again: confirm a valid assembly.
- Sneak and tap the Ship Core: cancel.
- Walking more than 24 blocks away also cancels.

## Expected visuals

- Cyan particles mark exposed faces of connected ship blocks.
- A cyan particle boundary box surrounds a valid detected ship.
- Helm blocks pulse amber.
- Aero Engine blocks pulse orange.
- Invalid structures use a red boundary and red exposed-face markers.
- The action bar shows block count, estimated mass, engine count, and controls.
- Chat shows block count, dimensions, mass, Helm count, and Engine count.

## Invalid tests

1. Remove the Aero Engine and tap the core. The preview must turn red and report a missing engine.
2. Reconnect the engine, add a second connected Ship Core, and scan again. It must reject multiple cores.
3. Connect the structure to the ground. The scan must stop at its 2,048-block or 64-block-axis safety limit and explain that the ship may touch terrain.
4. Restore a small, separated, valid ship and confirm it. Minecraft should show "Assembly confirmed."

## Honest boundary

Version 0.0.27 performs a real in-game scan and saves the confirmed core plus assembly summary. It does not remove blocks from the Minecraft grid or move the structure. Particle outlines are the phone-safe integration renderer for this milestone; native RenderDragon line submission remains under development.
