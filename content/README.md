# Bedrock Aeronautics content pack

This directory contains the original Bedrock Add-On definitions that accompany the native LeviPack.

Version 0.0.25 introduces three construction blocks:

- `aeronautics:ship_core`
- `aeronautics:helm`
- `aeronautics:aero_engine`

The first content milestone deliberately uses aliases to Minecraft's installed vanilla textures. No Mojang texture files are copied into this repository.

The blocks are currently construction and interaction anchors. Ship scanning, assembly, moving rendering, player reference frames, and native block-event integration remain later milestones. The C++ autopilot controller is compiled and host-tested in this version, but is not yet connected to a live assembled ship.

GitHub Actions packages the behavior and resource packs as `bedrock-aeronautics-content-0.0.25.mcaddon`.
