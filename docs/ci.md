# Continuous Integration

## Validate workflow

Runs on pushes, pull requests, and manual dispatches. It builds the platform-independent module on Ubuntu, executes the host contract test, validates the manifest generator, and rejects tracked APKs, native libraries, signing stores, and `libminecraftpe.so`.

## Android ARM64 workflow

Runs on `main`, `development`, pull requests, and manual dispatches.

Pinned build inputs:

- Android NDK `27.2.12479018`
- CMake `3.22.1`
- ABI `arm64-v8a`
- Android API 26
- C++20

The workflow verifies the output as AArch64, checks the exported loader symbols, retains an unstripped debug library and linker map, creates a stripped test module, generates build metadata, and uploads checksums with the artifact.

## Stability rule

A green Actions run proves only that the module builds and satisfies static checks. It does not prove that Minecraft can load it. Runtime acceptance requires evidence from the target Android device and exact Minecraft 1.21.0.03 installation.
