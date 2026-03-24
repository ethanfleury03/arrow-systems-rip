#!/usr/bin/env python3
"""
Blender headless renderer entrypoint for a box-only mockup MVP.

Usage (from CLI):
  blender -b -P scripts/blender/render_box_mockup.py -- --job scripts/blender/examples/box_job.example.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render a box label mockup in Blender (MVP scaffold)")
    parser.add_argument("--job", required=True, help="Path to job JSON")
    return parser.parse_args(argv)


def load_job(job_path: Path) -> dict:
    if not job_path.exists():
        raise FileNotFoundError(f"Job file not found: {job_path}")

    with job_path.open("r", encoding="utf-8") as f:
        data = json.load(f)

    required = ["job_id", "scene", "box", "label", "output"]
    missing = [k for k in required if k not in data]
    if missing:
        raise ValueError(f"Job file missing required keys: {', '.join(missing)}")

    return data


def ensure_output_dir(job: dict) -> None:
    output_path = Path(job["output"]["image_path"])
    output_path.parent.mkdir(parents=True, exist_ok=True)


def render_with_blender(job: dict) -> None:
    try:
        import bpy  # type: ignore
    except ImportError as e:
        raise RuntimeError(
            "This script must be executed inside Blender's Python runtime. "
            "Use the helper script: scripts/blender/run_blender_headless.sh"
        ) from e

    # MVP placeholder pipeline:
    # 1) Reset scene
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # 2) TODO: Build a simple cube with box dimensions and apply label texture.
    # This scaffold intentionally keeps scene logic minimal until assets are finalized.

    # 3) Placeholder camera/light setup
    bpy.ops.object.camera_add(location=(2.5, -2.5, 2.0), rotation=(1.1, 0, 0.8))
    camera = bpy.context.object
    bpy.context.scene.camera = camera

    bpy.ops.object.light_add(type="AREA", location=(1.5, -1.5, 3.0))

    # 4) Render output
    output_path = Path(job["output"]["image_path"]).resolve()
    bpy.context.scene.render.filepath = str(output_path)
    bpy.context.scene.render.image_settings.file_format = "PNG"

    # Keep fast defaults for MVP
    bpy.context.scene.render.engine = "BLENDER_EEVEE"
    bpy.context.scene.render.resolution_x = int(job["output"].get("resolution", {}).get("width", 1280))
    bpy.context.scene.render.resolution_y = int(job["output"].get("resolution", {}).get("height", 720))

    bpy.ops.render.render(write_still=True)


def main() -> int:
    # Blender passes custom args after "--"
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []

    try:
        args = parse_args(argv)
        job_path = Path(args.job)
        job = load_job(job_path)
        ensure_output_dir(job)
        render_with_blender(job)

        print(f"[ok] Render complete: {job['output']['image_path']}")
        return 0
    except Exception as e:
        print(f"[error] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
