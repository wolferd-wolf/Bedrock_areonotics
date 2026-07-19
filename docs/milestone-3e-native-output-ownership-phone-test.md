# Milestone 3E phone test — native helper output ownership

Target: Minecraft Bedrock Android 1.26.33.1, ARM64, with LeviLauncher 1.5.5.

This is a read-only native diagnostic. No native cube or moving block mesh should appear in this version.

## Install

1. Back up the test world.
2. Replace the previous native module with the version 0.0.30 LeviPack.
3. Remove the older Bedrock Aeronautics behavior and resource packs from Minecraft storage.
4. Open `bedrock-aeronautics-content-0.0.30.mcaddon` with Minecraft.
5. Activate both Bedrock Aeronautics packs in a Creative test world.
6. Start Minecraft through LeviLauncher.

## Thirty-minute stability run

1. Remain in the world for five minutes while slowly looking around.
2. Walk and fly around loaded terrain for five minutes.
3. Scan and confirm a small disconnected ship with the Ship Core.
4. Engage the connected Helm, let the blue proxy reach hover, then sneak to land.
5. Repeat the lift, hover, and landing cycle twice.
6. After landing, enter another dimension, move and look around, then return.
7. Continue normal movement until the total session is at least thirty minutes.
8. Exit the world normally, disable the native mod, and close Minecraft normally.

The existing blue particle proxy should still work. The original blocks remain stationary. A native cube is not expected.

## Files to send back

From the Bedrock Aeronautics LeviLauncher mod data directory, send:

- `terrain-command-ownership-status.txt`
- `terrain-command-ownership-timeline.csv`

Do not rename or edit them.

## Evidence gates

The run is useful when the status file reports:

- `schema=12`
- `source=terrain_helper_output_ownership_census`
- all three hook fingerprints validated;
- terrain helper calls inside the terrain task;
- `in_place_payload_observation_calls` greater than zero;
- at least one successful destination snapshot;
- zero pending-overwrite calls;
- normal shutdown with hook restoration and safe unload.

A changed in-place payload mask or changed destination mask identifies the next structure to decode. If both remain zero, or destination snapshots fail, geometry submission remains disabled and the next build will narrow the hook ABI instead.

## Crash rule

If Minecraft crashes, stop testing and send the two files if they were written, plus the LeviLauncher crash log. Do not repeat the crash loop.
