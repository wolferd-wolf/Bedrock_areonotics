# Bedrock content pack

This directory contains the Bedrock Aeronautics behavior and resource packs.

Version 0.0.27 includes:

- original 64x64 Ship Core, Helm, and Aero Engine textures;
- a stable `@minecraft/server` 2.1.0 script module;
- live Ship Core interaction and six-face connected-block scanning;
- cyan/red assembly boundary and exposed-face particles;
- amber Helm and orange Aero Engine markers;
- action-bar, title, chat, confirm, cancel, distance, and timeout behavior.

The script entry point is `behavior_packs/bedrock_aeronautics_bp/scripts/main.js`. The pure deterministic scanner lives in `ship_scan.js` and is tested independently with Node.

The outline particles use Minecraft's installed particle atlas through a resource-pack reference; the repository does not copy Mojang texture binaries.

The block textures are original to Bedrock Aeronautics. Do not add APK content, extracted proprietary game assets, or credentials to this repository.
