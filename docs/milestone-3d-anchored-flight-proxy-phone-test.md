# Milestone 3D phone test — anchored flight proxy

Target: Minecraft Bedrock Android 1.26.33.1 with LeviLauncher.

## Install

1. Back up the test world.
2. Replace the previous native module with the version 0.0.29 LeviPack.
3. Remove the older Bedrock Aeronautics behavior and resource packs from Minecraft storage.
4. Open the version 0.0.29 `.mcaddon` with Minecraft.
5. Activate both Bedrock Aeronautics packs in a Creative test world.
6. Experimental toggles are not required.

## Build the test ship

Give yourself the controls:

```mcfunction
/give @s aeronautics:ship_core
/give @s aeronautics:helm
/give @s aeronautics:aero_engine
```

Build a small ship with exactly one Ship Core, at least one Helm, one Aero Engine, and several ordinary blocks. Leave at least eight blocks of open air above the Helm. Break the final support so the assembly no longer touches the ground or terrain.

## Confirm and launch

1. Tap the Ship Core once. The assembly preview must appear.
2. Tap the Ship Core again. The message must say `Assembly confirmed` and tell you to tap a connected Helm.
3. Stand normally and tap the connected Helm.
4. A short-lived blue outline of the complete detected assembly must separate from the original blocks and rise.
5. Your player must remain anchored one block above the moving Helm.
6. The action bar must show `ASCENDING`, altitude, and vertical speed.
7. The blue proxy must settle near 4.00 m and change to `HOVERING`.
8. Sneak once. The action bar must change to `RETURNING`.
9. The proxy and pilot must descend together and stop at the original Helm.
10. Minecraft must report `Flight proxy landed`.

## Repeat and safety tests

- Tap the Helm again after landing. A second lift/hover/return cycle must work.
- Hold the interaction button on the Helm. One press must not engage multiple cycles.
- Try a different Helm that was not in the confirmed assembly. It must ask you to confirm that ship first.
- Build an intentionally very heavy assembly with too few engines. The Helm must report insufficient thrust instead of launching.
- Leave the dimension during a test only after landing; dimension-change recovery for active moving ships is not complete.

## Expected visual boundary

The original Minecraft blocks remain stationary and visible. The moving blue outline is the version 0.0.29 reference-frame proxy. Cyan/red particles are still used for the stationary assembly preview; amber and orange markers identify the moving Helm and Aero Engines.

## Honest boundary

This version proves transformed whole-assembly rendering, fixed-step lift/hover/return physics, engine thrust versus mass, and an anchored pilot. It does not yet move the real block mesh, collide the proxy with terrain, or allow the player to walk inside the moving reference frame.
