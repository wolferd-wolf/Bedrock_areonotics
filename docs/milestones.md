# Development Milestones

## Milestone 0 — Repository bootstrap

Acceptance criteria:

- Host C++ build succeeds.
- Host contract test succeeds.
- Android ARM64 shared library builds in GitHub Actions.
- ELF machine is verified as AArch64.
- Required exported symbols are verified.
- Artifact includes stripped and debug libraries, linker map, manifest, export list, and checksums.

## Milestone 1 — Native loader proof

- Load the Actions-built module through the selected Android loader.
- Confirm the startup diagnostic in device logs.
- Confirm Minecraft reaches the menu and loads a world without crashing.
- Record exact APK fingerprint and loader version.

## Milestone 2 — Safe tick hook

- Add a central version profile and symbol validation.
- Hook one update callback.
- Count ticks with throttled diagnostics.
- Run for at least ten minutes without a crash.

## Milestone 3 — Render hook

- Render a diagnostic triangle.
- Render one camera-relative cube.
- Add a safe runtime toggle.

## Milestone 4 — Transformed platform

- Render a small block platform in local coordinates.
- Translate and rotate it as one rigid structure.

## Later sequence

1. Ship section storage
2. Assembly and disassembly
3. Rigid-body movement
4. Propulsion and lift
5. Compound collision
6. Players on moving decks
7. Transformed block interaction
8. Original kinetic machinery
9. Damage and persistence
10. Multiplayer

No milestone advances until the preceding acceptance criteria have evidence.
