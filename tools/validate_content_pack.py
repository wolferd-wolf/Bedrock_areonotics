#!/usr/bin/env python3
"""Validate the original Bedrock Aeronautics content-pack contract."""

from __future__ import annotations

import json
import pathlib
import struct
import sys


EXPECTED_BLOCKS = {
    "aeronautics:ship_core",
    "aeronautics:helm",
    "aeronautics:aero_engine",
}
EXPECTED_TEXTURES = {
    "aeronautics_ship_core": "aeronautics_ship_core.png",
    "aeronautics_helm": "aeronautics_helm.png",
    "aeronautics_aero_engine": "aeronautics_aero_engine.png",
}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def fail(message: str) -> None:
    raise SystemExit(f"content validation failed: {message}")


def load_json(path: pathlib.Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{path}: {error}")


def validate_png(path: pathlib.Path) -> None:
    try:
        header = path.read_bytes()[:24]
    except OSError as error:
        fail(f"{path}: {error}")
    if len(header) != 24 or header[:8] != PNG_SIGNATURE:
        fail(f"{path}: invalid PNG signature or IHDR")
    width, height = struct.unpack(">II", header[16:24])
    if (width, height) != (64, 64):
        fail(f"{path}: expected 64x64, got {width}x{height}")


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
    if textures != set(EXPECTED_TEXTURES):
        fail(f"texture alias mismatch: {sorted(textures)}")

    terrain = load_json(rp / "textures" / "terrain_texture.json")
    texture_data = terrain.get("texture_data", {})
    if not textures.issubset(texture_data):
        fail("one or more block texture aliases are undefined")

    texture_root = rp / "textures" / "blocks"
    expected_png_paths: set[pathlib.Path] = set()
    for alias, filename in EXPECTED_TEXTURES.items():
        expected_path = f"textures/blocks/{pathlib.Path(filename).stem}"
        actual_path = texture_data.get(alias, {}).get("textures")
        if actual_path != expected_path:
            fail(f"{alias}: expected original texture path {expected_path}")
        png_path = texture_root / filename
        validate_png(png_path)
        expected_png_paths.add(png_path)

    tracked_png_paths = set(root.rglob("*.png"))
    if tracked_png_paths != expected_png_paths:
        unexpected = sorted(str(path) for path in tracked_png_paths - expected_png_paths)
        missing = sorted(str(path) for path in expected_png_paths - tracked_png_paths)
        fail(f"original texture set mismatch; unexpected={unexpected}; missing={missing}")

    forbidden = [
        path for path in root.rglob("*")
        if path.suffix.lower() in {".tga", ".jpg", ".jpeg"}
    ]
    if forbidden:
        fail("repository contains an unexpected raster asset format")

    print(
        "content validation passed; "
        f"json_files={len(json_files)}; blocks={len(identifiers)}; "
        f"original_textures={len(expected_png_paths)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
