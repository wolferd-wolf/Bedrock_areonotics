# Milestone 3C.1 phone test — repeatable Ship Core interaction

Target: Minecraft Bedrock Android 1.26.33.1 with LeviLauncher.

## Install

1. Back up the test world.
2. Replace the previous native module with the version 0.0.28 LeviPack.
3. Remove the older Bedrock Aeronautics behavior and resource packs from Minecraft storage.
4. Open the version 0.0.28 `.mcaddon` with Minecraft.
5. Activate both Bedrock Aeronautics packs in a Creative test world.
6. Experimental toggles are not required.

## Obtain the blocks

```mcfunction
/give @s aeronautics:ship_core
/give @s aeronautics:helm
/give @s aeronautics:aero_engine
```

Build a small connected ship with exactly one Ship Core, at least one Helm, at least one Aero Engine, and a few ordinary solid blocks. Break the final support so the ship is separated from the ground.

## Regression test

1. Tap the Ship Core once. A cyan valid preview, or a red invalid preview with a reason, must appear.
2. Tap the same core again. A valid preview must report `Assembly confirmed`.
3. Wait one second and tap the same core a third time. A new preview must start.
4. Tap it a fourth time. It must confirm again.
5. Press and hold on the core. One press must not trigger multiple confirmations.
6. Sneak-tap the core while a preview is active. The preview must cancel.
7. Tap after cancellation. A new preview must start.
8. Break and replace the core, then repeat steps 1–4.

## Expected result

Every separate tap is accepted. A held press or duplicate event is filtered. The core remains usable after preview, confirmation, cancellation, expiry, and replacement.

## Honest boundary

Version 0.0.28 fixes interaction reliability and retains the version 0.0.27 live scan and particle preview. It does not yet remove blocks from the Minecraft grid or move the assembled structure.
