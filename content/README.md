# Bedrock content pack

This directory contains the coordinated Bedrock Aeronautics behavior and resource packs.

Version 0.0.30 keeps the phone-tested 0.0.29 gameplay bridge intact:

- original 64x64 Ship Core, Helm, and Aero Engine textures;
- repeatable Ship Core and Helm interaction;
- live connected-block scanning and confirmation;
- the fixed 20 Hz anchored lift, hover, and return proxy;
- mass and Aero Engine thrust validation;
- the blue moving assembly outline and anchored pilot;
- the existing cyan/red assembly, amber Helm, and orange Aero Engine markers.

The new 0.0.30 milestone is in the native LeviPack. It performs a read-only census of Minecraft's terrain-helper output ownership so the project can identify the real command element before native block-mesh submission. The content pack remains coordinated at the same version so installation and diagnostics are unambiguous.

The script entry point is `behavior_packs/bedrock_aeronautics_bp/scripts/main.js`. Pure deterministic scanning, interaction gating, and flight physics live in `ship_scan.js`, `interaction_gate.js`, and `flight_proxy.js`; all are tested independently with Node.

The particle effects use Minecraft's installed particle atlas through resource-pack references. The repository does not copy Mojang texture binaries.

The block textures are original to Bedrock Aeronautics. Do not add APK content, extracted proprietary game assets, or credentials to this repository.
