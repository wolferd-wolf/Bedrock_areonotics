# Milestone 2C.2 — Single Vtable-Slot Pointer Probe

## Device evidence

Build `0.0.7` validated all five selected neighbouring methods but every 4-byte `GlossHookInternal` installation was rejected. No candidate callback or timeline sampling occurred.

Build `0.0.8` changed those probes to full-size inline hooks. On the exact target device, Minecraft began loading and then exited immediately. The supplied live-log screenshots showed launcher, Vivo framework, Firebase, graphics-driver, and Android UI warnings, but did not include a native tombstone or a Bedrock Aeronautics stack trace. The meaningful regression boundary is that the exit first appeared when five Minecraft function prologues were rewritten.

The 0.0.8 inline candidate method is rejected. The proven heartbeat detour remains the stable baseline.

## 0.0.9 design

Build `0.0.9` probes one candidate only:

- vtable start: `0x140545a0`;
- candidate slot: `160`;
- slot byte offset: `1280`;
- slot module-relative address: `0x14054aa0`;
- original function target: `0x9d82094`.

The module waits at least eight seconds and requires a live menu-state observer before attempting the replacement. It then temporarily replaces the aligned 64-bit slot pointer with an ARM64 trampoline. No instruction in `libminecraftpe.so` is rewritten.

The trampoline preserves:

- integer argument and scratch registers `x0` through `x18`;
- link register `x30`;
- SIMD/floating-point argument registers `q0` through `q7`;
- `NZCV`, `FPCR`, and `FPSR`.

After an atomic counter update, it restores the incoming state and tail-branches to the original slot-160 method.

## Compatibility gates

The pointer replacement is refused unless all of these match:

1. Minecraft version `1.26.33.1`.
2. GNU build ID `2e318db12824cadb2618754ab7c82fa96fb30659`.
3. Module file size `349243744`.
4. Heartbeat target offset `0x9d80fac`.
5. Vtable slot 152 points to the proven heartbeat.
6. Slot 160 points to executable target `0x9d82094`.
7. The slot is aligned and contained in a readable, non-executable mapping.
8. The original page protection can be determined from `/proc/self/maps`.

## Restoration rules

The original page protection is restored immediately after each pointer write. On disable or unload, the module restores the original slot value with up to three attempts. If the slot still references the module trampoline, unload is refused so Minecraft cannot retain a dangling pointer into an unloaded module.

## Output

Summary profile:

```text
<mod data directory>/vtable-probe-profile.txt
```

Two-second timeline:

```text
<mod data directory>/vtable-probe-timeline.txt
```

The summary uses schema 6. A successful clean shutdown should contain:

```text
schema=6
state=stopped
discovery_method=vtable_slot_pointer_trampoline
patch_mode=data_pointer_swap_no_minecraft_code_patch
minecraft_code_bytes_modified=0
patch_ever_installed=true
patch_currently_installed=false
slot_current_state=original
patch_restore_succeeded=true
```

## Device test

1. Disable or remove build `0.0.8`.
2. Import and enable build `0.0.9`.
3. Use a disposable world.
4. Stay on the main menu for 20–30 seconds.
5. Enter the world and remain idle for 30 seconds.
6. Walk, jump, place blocks, and break blocks for 30 seconds.
7. Open the pause menu for 20 seconds.
8. Resume gameplay for 20 seconds.
9. Leave the world for 20 seconds.
10. Re-enter briefly and exit Minecraft normally.
11. Provide both complete diagnostic files.

If Minecraft exits or crashes, do not repeatedly relaunch the build. Preserve the first tombstone or xCrash file and the complete LeviLauncher log. Generic startup-warning screenshots alone are insufficient to identify a native fault address.
