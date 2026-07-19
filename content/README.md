# Bedrock content pack

This directory contains the Bedrock Aeronautics behavior and resource packs.

Version 0.0.28 includes:

- original 64x64 Ship Core, Helm, and Aero Engine textures;
- a stable `@minecraft/server` 2.1.0 script module;
- repeatable Ship Core interaction using a cancellable before-event bridge;
- next-tick six-face connected-block scanning outside restricted execution;
- held-press and duplicate-event filtering that still accepts later taps;
- cyan/red assembly boundary and exposed-face particles;
- amber Helm and orange Aero Engine markers;
- action-bar, title, chat, confirm, cancel, distance, and timeout behavior.

The script entry point is `behavior_packs/bedrock_aeronautics_bp/scripts/main.js`. The pure deterministic scanner lives in `ship_scan.js`. The re-entry gate lives in `interaction_gate.js`; both are tested independently with Node.

The outline particles use Minecraft's installed particle atlas through a resource-pack reference; the repository does not copy Mojang texture binaries.

The block textures are original to Bedrock Aeronautics. Do not add APK content, extracted proprietary game assets, or credentials to this repository.
