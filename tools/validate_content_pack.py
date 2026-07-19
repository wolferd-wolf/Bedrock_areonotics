#!/usr/bin/env python3
"""Validate the original Bedrock Aeronautics content-pack contract."""

from __future__ import annotations

import json
import pathlib
import sys


EXPECTED_BLOCKS = {
    "aeronautics:ship_core",
    "aeronautics:helm",
    "aeronautics:aero_engine",
}


def fail(message: str) -> None:
    raise SystemExit(f"content validation failed: {message}")


def load_json(path: pathlib.Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{path}: {error}")


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "content")
    if not root.is_dir():
        fail(f"missing content root: {root}")

    json_files = sorted(root.rglob("*.json"))
    if not json_files:
        fail("no JSON files found")
    for path in json_files:
        load_json(path)

    bp = root / "behavior_packs" / "bedrock_aeronautics_bp"
    rp = root / "resource_packs" / "bedrock_aeronautics_rp"
    bp_manifest = load_json(bp / "manifest.json")
    rp_manifest = load_json(rp / "manifest.json")

    bp_dependencies = {
        item.get("uuid")
        for item in bp_manifest.get("dependencies", [])
        if isinstance(item, dict)
    }
    rp_uuid = rp_manifest.get("header", {}).get("uuid")
    if rp_uuid not in bp_dependencies:
        fail("behavior pack does not depend on the resource pack")

    identifiers: set[str] = set()
    textures: set[str] = set()
    for path in sorted((bp / "blocks").glob("*.json")):
        document = load_json(path)
        block = document.get("minecraft:block", {})
        description = block.get("description", {})
        identifier = description.get("identifier")
        if not isinstance(identifier, str) or not identifier.startswith("aeronautics:"):
            fail(f"{path}: invalid namespaced identifier")
        identifiers.add(identifier)

        components = block.get("components", {})
        if "minecraft:geometry" not in components:
            fail(f"{path}: geometry component missing")
        material_instances = components.get("minecraft:material_instances", {})
        texture = material_instances.get("*", {}).get("texture")
        if not isinstance(texture, str):
            fail(f"{path}: material texture missing")
        textures.add(texture)
        if "minecraft:item_visual" not in components:
            fail(f"{path}: item visual missing")
        if components.get("minecraft:collision_box") is not True:
            fail(f"{path}: collision box must be enabled")

    if identifiers != EXPECTED_BLOCKS:
        fail(f"block set mismatch: {sorted(identifiers)}")

    terrain = load_json(rp / "textures" / "terrain_texture.json")
    texture_data = terrain.get("texture_data", {})
    if not textures.issubset(texture_data):
        fail("one or more block texture aliases are undefined")
    for name in textures:
        texture_path = texture_data[name].get("textures", "")
        if not texture_path.startswith("textures/blocks/"):
            fail(f"{name}: texture must resolve through the installed block atlas")

    forbidden = [
        path for path in root.rglob("*")
        if path.suffix.lower() in {".png", ".tga", ".jpg", ".jpeg"}
    ]
    if forbidden:
        fail("repository must not copy proprietary texture binaries")

    print(
        "content validation passed; "
        f"json_files={len(json_files)}; blocks={len(identifiers)}; "
        f"texture_aliases={len(textures)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
