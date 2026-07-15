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
- Output confirmed as ELF 64-bit ARM AArch64.
- Debug library contains debug information and is not stripped.
- Distribution library is stripped.
- Linker map, ELF header, export list, build manifest, and SHA-256 checksums are present.
- Every packaged checksum verified successfully.

Milestone status:

> Milestone 0 acceptance criteria satisfied and merged into `main` through pull request #1.

## 2026-07-15 — Milestone 1 loader-proof package

Architecture correction:

- Replaced the placeholder `mod_init()` convention with LeviLauncher's supported `preloader-android` lifecycle ABI.
- Pinned `LiteLDev/preloader-android` version `0.2.3`.
- Added `PL_REGISTER_MOD` and the exported `PLGetModRegistration` symbol.
- Added conservative load, enable, disable, and unload diagnostics.
- Confirmed Amethyst is not being used as the Android loader; it remains reference material only.

Build and packaging changes:

- Updated the Android toolchain to NDK `28.2.13676358` and API 28.
- Added 16 KiB maximum page-size linker compatibility.
- Added a version-locked LeviLauncher `preload-native` manifest.
- Added `.levipack` generation.
- Added complete configure and compile-log artifacts on failure.

CI debugging record:

- Android run `29387320245` failed during final module linking.
- Uploaded failure artifact `8331935738` exposed an undefined `__android_log_print` symbol.
- Root cause: the preloader logger is inline in module translation units, while the preloader target links `liblog` privately.
- Corrective action: link Android `liblog` directly into `libbedrock_aeronautics.so`.

### Superseded 1.21 loader package

- The initial loader package was locked to Minecraft `1.21.0.03`.
- The target phone cannot use that older Minecraft build through the current LeviLauncher setup.
- That package must not be imported into the active 1.26.33.1 instance.

### Active 1.26.33.1 port

- Retargeted the loader-proof manifest, runtime diagnostics, build metadata, CI assertions, architecture document, and phone-test checklist to Minecraft Bedrock Android `1.26.33.1`.
- Kept the package exact-version locked rather than using a broad `1.26.*` wildcard.
- No Minecraft functions are hooked in this package.
- Future tick/render development requires a fresh 1.26.33.1 signature and type profile; 1.21 definitions are not reusable without verification.

Successful 1.26.33.1 CI evidence:

- Host validation run `29407862769`: success.
- Android loader-package run `29407862752`: success.
- Artifact `8339850784` downloaded and independently inspected.
- Output is an ELF 64-bit ARM AArch64 shared object for Android API 28, built with NDK r28c.
- Distribution library is stripped; debug library retains debug information.
- Required exports are present:
  - `PLGetModRegistration`
  - `bedrock_aeronautics_version`
- Required dynamic dependencies include `libpreloader.so` and `liblog.so`.
- `bedrock_aeronautics.levipack` contains:
  - `bedrock_aeronautics/manifest.json`
  - `bedrock_aeronautics/libbedrock_aeronautics.so`
- Manifest and build metadata are locked to Minecraft `1.26.33.1`.
- All artifact and package SHA-256 checksums verified successfully.

Successful target-device evidence:

- User imported the 1.26.33.1 `.levipack` through LeviLauncher.
- Minecraft reached and remained stable inside a world.
- LeviLauncher log overlay showed `Bedrock Aeronautics native module loaded`.
- Loaded version was `0.0.1-dev+ba15970`.
- Module directory resolved under LeviLauncher's Minecraft mod directory.
- LeviLauncher log overlay showed `Bedrock Aeronautics enabled`.
- The lifecycle sequence appeared on two separate launches roughly ten minutes apart.
- No import rejection, native crash, repeated restart, or world-load failure was observed.

Milestone status:

> Milestone 1 acceptance criteria satisfied on the target Android phone. Pull request #2 is ready to merge.

Next milestone:

- Create a dedicated 1.26.33.1 compatibility profile.
- Capture the exact Minecraft binary fingerprint and loader version.
- Research and validate one safe update/tick callback.
- Add a throttled diagnostic counter without touching rendering or world state.
