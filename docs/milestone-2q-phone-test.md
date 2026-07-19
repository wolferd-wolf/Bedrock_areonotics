# Milestone 2Q phone test — terrain command payload decoding

## Build

- Version: 0.0.24
- Target: Minecraft Bedrock Android 1.26.33.1 ARM64
- Expected artifact prefix: `bedrock-aeronautics-m2q-terrain-command-payload`
- Visible geometry: none; this remains a read-only discovery build

## Procedure

1. Replace the previous module with the complete 0.0.24 artifact and launch Minecraft through LeviLauncher.
2. Enter an existing Overworld world and wait at least 30 seconds after terrain is visible.
3. Walk or fly across several loaded chunks for two minutes. Look toward both nearby terrain and the horizon.
4. Pause for 20 seconds, then move again for another minute.
5. Leave the world and close Minecraft normally so the final `stopped` status is written.
6. Collect both files from the mod data directory:
   - `terrain-command-payload-status.txt`
   - `terrain-command-payload-timeline.csv`

## Success criteria

The status file should report:

- `schema=11`
- `source=terrain_command_payload_decoding`
- `fingerprint_validated=true`
- all three prefix validations as `true`
- `terrain_helper_inside_task_calls` greater than zero
- `descriptor_sample_count=16`
- `payload_capture_calls` greater than zero
- `payload_pending_overwrite_calls=0`
- per-descriptor first and last qwords plus field masks
- `geometry_submission=none_discovery_only`
- `failure_reason=none`

A non-zero payload capture failure count is not automatically a crash or failure. It means one helper call could not prove a complete 64-byte range inside the vector. The aggregate and per-descriptor counts will tell us whether that helper uses a different construction path.

## Stop conditions

Stop testing and preserve the latest files if Minecraft crashes, the world becomes visually corrupted, input freezes, or the status reports a fingerprint or prefix mismatch. Do not combine files from 0.0.23 and 0.0.24.
