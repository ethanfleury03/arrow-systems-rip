#!/usr/bin/env python3
"""
Blender headless renderer entrypoint for a box-only mockup MVP.

Usage (from CLI):
  blender -b -P scripts/blender/render_box_mockup.py -- --job scripts/blender/examples/box_job.example.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve()
DEFAULT_REPO_ROOT = SCRIPT_PATH.parents[2]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render a box label mockup in Blender")
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


def find_repo_root(start: Path) -> Path:
    for parent in [start, *start.parents]:
        if (parent / ".git").exists():
            return parent
    return DEFAULT_REPO_ROOT


def candidate_bases(job_path: Path) -> list[Path]:
    bases = [job_path.parent, find_repo_root(job_path), DEFAULT_REPO_ROOT, Path.cwd()]
    unique: list[Path] = []
    seen: set[str] = set()
    for b in bases:
        ab = b.absolute()
        key = str(ab)
        if key not in seen:
            seen.add(key)
            unique.append(ab)
    return unique


def hex_to_rgba(color: str, alpha: float = 1.0) -> tuple[float, float, float, float]:
    value = color.lstrip("#")
    if len(value) != 6:
        return (0.9, 0.85, 0.75, alpha)
    r = int(value[0:2], 16) / 255.0
    g = int(value[2:4], 16) / 255.0
    b = int(value[4:6], 16) / 255.0
    return (r, g, b, alpha)


def mm_to_m(v: float) -> float:
    return float(v) / 1000.0


def get_dimensions_m(job: dict) -> tuple[float, float, float]:
    dims = job.get("box", {}).get("dimensions_mm", {})
    width = mm_to_m(dims.get("width", 120))
    height = mm_to_m(dims.get("height", 180))
    depth = mm_to_m(dims.get("depth", 60))
    if width <= 0 or height <= 0 or depth <= 0:
        raise ValueError("box.dimensions_mm width/height/depth must all be > 0")
    return width, height, depth


def resolve_input_path(raw_path: str, job_path: Path) -> Path:
    candidate = Path(raw_path).expanduser()
    if candidate.is_absolute():
        return candidate

    # Prefer paths relative to the job file, then discovered repo root, then cwd (legacy).
    for base in candidate_bases(job_path):
        resolved = (base / candidate).resolve()
        if resolved.exists():
            return resolved

    return (job_path.parent / candidate).resolve()


def absolutize_output_path(path: Path, job_path: Path) -> Path:
    if path.is_absolute():
        return path
    return (job_path.parent / path).absolute()


def resolve_render_paths(job: dict, job_path: Path) -> dict[str, Path]:
    output = job.get("output", {})
    views = output.get("views")
    if isinstance(views, dict) and views:
        paths = {name: absolutize_output_path(Path(path), job_path) for name, path in views.items()}
        required = {"front", "angle", "closeup"}
        missing = required - set(paths.keys())
        if missing:
            raise ValueError(f"output.views missing keys: {', '.join(sorted(missing))}")
        return {k: paths[k] for k in ["front", "angle", "closeup"]}

    # Backward compatible path expansion from legacy single image_path
    base = absolutize_output_path(Path(output.get("image_path", "renders/box-mockup-mvp-001.png")), job_path)
    stem = base.stem
    suffix = base.suffix or ".png"
    parent = base.parent
    return {
        "front": parent / f"{stem}-front{suffix}",
        "angle": parent / f"{stem}-angle{suffix}",
        "closeup": parent / f"{stem}-closeup{suffix}",
    }


def ensure_output_dirs(job: dict, job_path: Path) -> dict[str, Path]:
    paths = resolve_render_paths(job, job_path)
    for p in paths.values():
        p.parent.mkdir(parents=True, exist_ok=True)
    return paths


def build_cardboard_material(bpy, job: dict):
    material = bpy.data.materials.new(name="CardboardMaterial")
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    box_material = job.get("box", {}).get("material", {})
    base_color = hex_to_rgba(box_material.get("base_color", "#d8c7a6"))
    roughness = float(box_material.get("roughness", 0.7))
    bsdf.inputs["Base Color"].default_value = base_color
    bsdf.inputs["Roughness"].default_value = max(0.0, min(1.0, roughness))
    bsdf.inputs["Specular IOR Level"].default_value = 0.2
    return material


def set_enum_if_supported(target, attr: str, value: str) -> None:
    if not hasattr(target, attr):
        return

    try:
        bl_rna = getattr(target, "bl_rna", None)
        prop = bl_rna.properties.get(attr) if bl_rna else None
        if prop and hasattr(prop, "enum_items"):
            valid_values = {item.identifier for item in prop.enum_items}
            if value not in valid_values:
                return
        setattr(target, attr, value)
    except Exception:
        # Ignore Blender version/API differences and keep rendering.
        pass


def build_label_material(bpy, label_path: Path):
    material = bpy.data.materials.new(name="LabelMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output_node = nodes.new(type="ShaderNodeOutputMaterial")
    output_node.location = (500, 0)

    bsdf = nodes.new(type="ShaderNodeBsdfPrincipled")
    bsdf.location = (220, 0)
    bsdf.inputs["Roughness"].default_value = 0.35

    texture = nodes.new(type="ShaderNodeTexImage")
    texture.location = (-200, 40)
    texture.image = bpy.data.images.load(str(label_path))

    links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
    if "Alpha" in texture.outputs and "Alpha" in bsdf.inputs:
        links.new(texture.outputs["Alpha"], bsdf.inputs["Alpha"])
        # Blender material transparency/shadow options have changed across versions.
        set_enum_if_supported(material, "blend_method", "BLEND")
        set_enum_if_supported(material, "surface_render_method", "BLENDED")
        set_enum_if_supported(material, "shadow_method", "CLIP")

    links.new(bsdf.outputs["BSDF"], output_node.inputs["Surface"])
    return material


def create_box_and_label(bpy, job: dict, label_path: Path):
    width, height, depth = get_dimensions_m(job)

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, height / 2.0))
    box_obj = bpy.context.object
    box_obj.name = "MockupBox"
    box_obj.scale = (width / 2.0, depth / 2.0, height / 2.0)

    cardboard_material = build_cardboard_material(bpy, job)
    box_obj.data.materials.clear()
    box_obj.data.materials.append(cardboard_material)

    label_cfg = job.get("label", {})
    placement = label_cfg.get("placement", {})
    scale_cfg = label_cfg.get("scale", {})

    center_x = float(placement.get("center_x", 0.5))
    center_y = float(placement.get("center_y", 0.5))
    scale_x = float(scale_cfg.get("width", 0.8))
    scale_y = float(scale_cfg.get("height", 0.8))

    center_x = max(0.0, min(1.0, center_x))
    center_y = max(0.0, min(1.0, center_y))
    scale_x = max(0.05, min(1.0, scale_x))
    scale_y = max(0.05, min(1.0, scale_y))

    label_w = width * scale_x
    label_h = height * scale_y

    # Front face is +Y in this setup.
    left = -width / 2.0
    bottom = 0.0
    x = left + center_x * width
    z = bottom + center_y * height
    y = (depth / 2.0) + 0.0008  # tiny offset to avoid z-fighting

    bpy.ops.mesh.primitive_plane_add(size=1.0, location=(x, y, z), rotation=(1.5707963, 0.0, 0.0))
    label_obj = bpy.context.object
    label_obj.name = "LabelFront"
    label_obj.scale = (label_w / 2.0, label_h / 2.0, 1.0)

    label_material = build_label_material(bpy, label_path)
    label_obj.data.materials.clear()
    label_obj.data.materials.append(label_material)

    return box_obj, label_obj, (width, height, depth)


def setup_lighting_and_world(bpy) -> None:
    world = bpy.data.worlds.get("World")
    if world:
        world.use_nodes = True
        bg = world.node_tree.nodes.get("Background")
        if bg:
            bg.inputs[0].default_value = (0.95, 0.95, 0.95, 1.0)
            bg.inputs[1].default_value = 0.9

    bpy.ops.object.light_add(type="AREA", location=(1.0, -1.4, 1.4))
    key = bpy.context.object
    key.data.energy = 900
    key.data.size = 1.0

    bpy.ops.object.light_add(type="AREA", location=(-1.0, -1.1, 0.9))
    fill = bpy.context.object
    fill.data.energy = 400
    fill.data.size = 1.3


def add_camera(bpy, name: str, location: tuple[float, float, float], rotation: tuple[float, float, float]):
    bpy.ops.object.camera_add(location=location, rotation=rotation)
    cam = bpy.context.object
    cam.name = name
    return cam


def setup_cameras(bpy, dims: tuple[float, float, float]):
    width, height, depth = dims
    cams = {
        "front": add_camera(
            bpy,
            "CamFront",
            (0.0, -(depth * 2.5 + 0.20), height * 0.55),
            (1.5707963, 0.0, 0.0),
        ),
        "angle": add_camera(
            bpy,
            "CamAngle",
            (width * 1.8 + 0.22, -(depth * 2.2 + 0.18), height * 0.75),
            (1.18, 0.0, 0.95),
        ),
        "closeup": add_camera(
            bpy,
            "CamCloseup",
            (0.0, -(depth * 1.15 + 0.06), height * 0.55),
            (1.5707963, 0.0, 0.0),
        ),
    }
    return cams


def configure_render_settings(bpy, job: dict):
    scene = bpy.context.scene
    output = job.get("output", {})
    res = output.get("resolution", {})
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.resolution_x = int(res.get("width", 1280))
    scene.render.resolution_y = int(res.get("height", 720))


def render_with_blender(job: dict, job_path: Path) -> dict[str, Path]:
    try:
        import bpy  # type: ignore
    except ImportError as e:
        raise RuntimeError(
            "This script must be executed inside Blender's Python runtime. "
            "Use: ./scripts/blender/run_blender_headless.sh <job-file.json>"
        ) from e

    label_path = resolve_input_path(str(job.get("label", {}).get("image_path", "")), job_path)
    if not label_path.exists():
        raise FileNotFoundError(f"Label image not found: {label_path}")

    render_paths = ensure_output_dirs(job, job_path)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    _, _, dims = create_box_and_label(bpy, job, label_path)
    setup_lighting_and_world(bpy)
    cams = setup_cameras(bpy, dims)
    configure_render_settings(bpy, job)

    scene = bpy.context.scene
    for view_name in ["front", "angle", "closeup"]:
        scene.camera = cams[view_name]
        scene.render.filepath = str(render_paths[view_name].resolve())
        bpy.ops.render.render(write_still=True)

    return render_paths


def main() -> int:
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []

    try:
        args = parse_args(argv)
        job_path = Path(args.job).expanduser()
        if not job_path.is_absolute():
            job_path = (Path.cwd() / job_path).absolute()
        job = load_job(job_path)
        outputs = render_with_blender(job, job_path)
        print("[ok] Render complete:")
        for key, path in outputs.items():
            print(f"  - {key}: {path}")
        return 0
    except Exception as e:
        print(f"[error] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
