# Progress Log

## 2026-07-15 — Milestone 0 bootstrap

Completed:

- Initialized private repository documentation and safety boundaries.
- Added minimal exported native module interface.
- Added Android/host diagnostic implementation.
- Added CMake C++20 shared-library target.
- Added host-side module contract test.
- Added host CMake presets.
- Added build-manifest generator.
- Added host validation workflow.
- Added pinned Android ARM64 workflow.
- Added architecture, CI, and milestone documentation.
- Created the protected development workflow through a draft pull request.

Validation evidence:

- Host validation run `29386532342`: success.
- Initial Android run failed before checkout because `actions/setup-java@v6` was not resolvable.
- Corrected the workflow to `actions/setup-java@v5` without changing the toolchain.
- Android ARM64 run `29386532335`: success.
- Artifact `8331643942` downloaded and inspected.
- Output confirmed as ELF 64-bit ARM AArch64 for Android API 26.
- Debug library contains debug information and is not stripped.
- Distribution library is stripped.
- Required exports `mod_init` and `bedrock_aeronautics_version` are present.
- Linker map, ELF header, export list, build manifest, and SHA-256 checksums are present.
- Every packaged checksum verified successfully.

Milestone status:

> Milestone 0 acceptance criteria satisfied. Proceed to Milestone 1 only after the CI correction is merged into `main`.

Device work required for Milestone 1:

- Install or confirm LeviLauncher-compatible loading environment.
- Install the exact Minecraft Bedrock Android 1.21.0.03 ARM64 build.
- Capture the APK and `libminecraftpe.so` SHA-256 fingerprints from the legally owned installation.
- Load `libbedrock_aeronautics.so`.
- Capture startup and crash logs.
- Confirm Minecraft reaches the menu and loads a world without a native crash.
