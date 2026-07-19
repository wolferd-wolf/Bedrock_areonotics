# Bedrock content pack

This directory contains the Bedrock Aeronautics behavior and resource packs.

Version 0.0.29 includes:

- original 64x64 Ship Core, Helm, and Aero Engine textures;
- stable `@minecraft/server` 2.1.0 scripting;
- repeatable Ship Core and Helm interaction through a cancellable before-event bridge;
- live connected-block scanning and confirmation;
- a fixed 20 Hz anchored vertical-flight physics proxy;
- mass and Aero Engine thrust validation;
- a short-lived blue particle renderer for the transformed assembly;
- pilot anchoring at the moving Helm;
- sneak-to-return control with a smooth landing;
- the existing cyan/red assembly, amber Helm, and orange Aero Engine markers.

The script entry point is `behavior_packs/bedrock_aeronautics_bp/scripts/main.js`. Pure deterministic scanning, interaction gating, and flight physics live in `ship_scan.js`, `interaction_gate.js`, and `flight_proxy.js`; all are tested independently with Node.

The particle effects use Minecraft's installed particle atlas through resource-pack references. The repository does not copy Mojang texture binaries.

The block textures are original to Bedrock Aeronautics. Do not add APK content, extracted proprietary game assets, or credentials to this repository.
