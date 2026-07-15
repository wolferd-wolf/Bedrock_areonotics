# Milestone 1 Device Test

This test proves only that LeviLauncher can import and execute the Bedrock Aeronautics lifecycle module on the exact target version. No Bedrock hooks are installed yet.

## Required environment

- Android ARM64 device
- LeviLauncher with native-mod support
- Legally owned Minecraft Bedrock Android 1.26.33.1 installation
- Version isolation enabled for the imported Minecraft instance when LeviLauncher requires it
- `bedrock_aeronautics.levipack` from the successful GitHub Actions artifact

## Preparation

1. Back up any important Minecraft worlds.
2. Use the LeviLauncher instance that reports Minecraft 1.26.33.1.
3. Confirm that the clean instance reaches the menu and loads a disposable test world before enabling the mod.
4. In LeviLauncher settings, enable the debug log or log overlay if available.
5. Grant overlay permission when LeviLauncher requests it; without this permission the log window may not be shown.

## Import

1. Open LeviLauncher.
2. Open the mod manager or import action.
3. Select `bedrock_aeronautics.levipack` built for Minecraft 1.26.33.1.
4. Confirm the imported mod is named **Bedrock Aeronautics**.
5. Confirm it is enabled for the Minecraft 1.26.33.1 instance.
6. Do not force-enable the package on another Minecraft build if LeviLauncher reports it as incompatible.

## Test sequence

1. Launch Minecraft and wait at the main menu for at least 30 seconds.
2. Confirm there is no startup crash or repeated restart.
3. Open a disposable test world.
4. Remain in the world for at least two minutes.
5. Leave the world and return to the menu.
6. Exit Minecraft normally.

## Expected lifecycle messages

The debug log should contain messages equivalent to:

```text
Bedrock Aeronautics native module loaded; version=...
Target: Minecraft Bedrock Android 1.26.33.1 ARM64
Module directory: ...
Bedrock Aeronautics enabled
```

Normal shutdown may additionally show disable and unload messages.

## Evidence to return

- LeviLauncher version
- Screenshot showing the Minecraft instance version as 1.26.33.1
- Screenshot of the imported mod entry
- Full debug log from launch through world exit
- Any Android native crash report
- Whether the clean instance worked before enabling the mod
- Exact step where a failure occurred

## Pass criteria

- `.levipack` imports without manifest rejection.
- LeviLauncher does not skip it as incompatible.
- Lifecycle load and enable messages appear.
- Minecraft reaches the main menu.
- A test world loads and remains stable for at least two minutes.
- Normal exit does not corrupt the isolated instance.

## Failure handling

Do not repeatedly relaunch a crashing build. Disable or remove the mod, confirm the clean instance still works, preserve the first complete log, and report the failure. The pull request must remain unmerged until the cause is understood.
