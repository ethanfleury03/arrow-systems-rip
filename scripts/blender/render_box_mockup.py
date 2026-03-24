#!/usr/bin/env python3
"""
Blender headless renderer entrypoint for box mockups.

Usage (from CLI):
  blender -b -P scripts/blender/render_box_mockup.py -- --job scripts/blender/examples/box_job.example.json
"""

from __future__ import annotations

import argparse
import json
import math
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


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def lerp_color(
    a: tuple[float, float, float, float],
    b: tuple[float, float, float, float],
    t: float,
) -> tuple[float, float, float, float]:
    tt = clamp(t, 0.0, 1.0)
    return (
        a[0] + (b[0] - a[0]) * tt,
        a[1] + (b[1] - a[1]) * tt,
        a[2] + (b[2] - a[2]) * tt,
        a[3] + (b[3] - a[3]) * tt,
    )


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


def compute_print_area_params(label_cfg: dict, image_aspect: float) -> dict:
    """Resolve Phase 3 print area with backwards compatibility for old placement/scale fields."""
    panel = str(label_cfg.get("target_face", "front")).lower()
    print_area = label_cfg.get("print_area") if isinstance(label_cfg.get("print_area"), dict) else {}

    panel = str(print_area.get("panel", panel)).lower()
    if panel not in {"front", "side", "top"}:
        panel = "front"

    rotation_deg = float(print_area.get("rotation", 0.0))
    scale_mode = str(print_area.get("scale_mode", "fit")).lower()
    if scale_mode not in {"fit", "fill"}:
        scale_mode = "fit"
    bleed = clamp(float(print_area.get("bleed", 0.0)), 0.0, 0.49)

    bounds = print_area.get("bounds") if isinstance(print_area.get("bounds"), dict) else None

    if bounds:
        x = float(bounds.get("x", 0.1))
        y = float(bounds.get("y", 0.1))
        w = float(bounds.get("width", 0.8))
        h = float(bounds.get("height", 0.8))
    elif any(k in print_area for k in ("x", "y", "width", "height")):
        x = float(print_area.get("x", 0.1))
        y = float(print_area.get("y", 0.1))
        w = float(print_area.get("width", 0.8))
        h = float(print_area.get("height", 0.8))
    else:
        # Legacy fallback from phase2 placement/scale
        placement = label_cfg.get("placement", {}) if isinstance(label_cfg.get("placement"), dict) else {}
        scale = label_cfg.get("scale", {}) if isinstance(label_cfg.get("scale"), dict) else {}
        cx = clamp(float(placement.get("center_x", 0.5)), 0.0, 1.0)
        cy = clamp(float(placement.get("center_y", 0.5)), 0.0, 1.0)
        sw = clamp(float(scale.get("width", 0.8)), 0.05, 1.0)
        sh = clamp(float(scale.get("height", 0.8)), 0.05, 1.0)
        x = cx - (sw / 2.0)
        y = cy - (sh / 2.0)
        w = sw
        h = sh

    x = clamp(x, 0.0, 1.0)
    y = clamp(y, 0.0, 1.0)
    w = clamp(w, 0.02, 1.0)
    h = clamp(h, 0.02, 1.0)

    # Keep rectangle in panel bounds
    if x + w > 1.0:
        w = max(0.02, 1.0 - x)
    if y + h > 1.0:
        h = max(0.02, 1.0 - y)

    # Expand/contract by bleed before fit/fill
    x = clamp(x - bleed, 0.0, 1.0)
    y = clamp(y - bleed, 0.0, 1.0)
    w = clamp(w + (2.0 * bleed), 0.02, 1.0 - x)
    h = clamp(h + (2.0 * bleed), 0.02, 1.0 - y)

    area_aspect = w / h
    if scale_mode == "fill":
        # cover area (some crop if needed)
        if image_aspect > area_aspect:
            # wider than area -> crop width
            content_h = h
            content_w = h * image_aspect
        else:
            # taller than area -> crop height
            content_w = w
            content_h = w / image_aspect
    else:
        # fit area (letterbox/pillarbox)
        if image_aspect > area_aspect:
            content_w = w
            content_h = w / image_aspect
        else:
            content_h = h
            content_w = h * image_aspect

    content_x = x + (w - content_w) * 0.5
    content_y = y + (h - content_h) * 0.5

    return {
        "panel": panel,
        "rotation_deg": rotation_deg,
        "content_x": content_x,
        "content_y": content_y,
        "content_w": content_w,
        "content_h": content_h,
    }


def parse_scene_background(job: dict) -> dict:
    """Parse new/legacy background fields with robust defaults."""
    scene_cfg = job.get("scene", {}) if isinstance(job.get("scene"), dict) else {}
    bg = scene_cfg.get("background", "studio_gray")

    # Legacy preset string support
    if isinstance(bg, str):
        key = bg.lower().strip()
        if key in {"studio_gray", "gray", "neutral", "default"}:
            return {
                "style": "auto_contrast_studio",
                "top_color": "#f1f3f6",
                "bottom_color": "#c8ced6",
                "floor_tint": "#d5dbe2",
                "floor_tint_intensity": 0.42,
            }
        if key in {"dark", "studio_dark"}:
            return {
                "style": "auto_contrast_studio",
                "top_color": "#dfe5ec",
                "bottom_color": "#b0b9c4",
                "floor_tint": "#c0c8d2",
                "floor_tint_intensity": 0.38,
            }
        if key in {"light", "studio_light"}:
            return {
                "style": "auto_contrast_studio",
                "top_color": "#f8f9fb",
                "bottom_color": "#d8dde4",
                "floor_tint": "#e3e7ed",
                "floor_tint_intensity": 0.33,
            }

    bg_obj = bg if isinstance(bg, dict) else {}

    style = str(bg_obj.get("style", "auto_contrast_studio")).lower().strip()
    if style not in {"auto_contrast_studio", "dual_tone", "flat"}:
        style = "auto_contrast_studio"

    top_color = str(bg_obj.get("top_color", "#f1f3f6"))
    bottom_color = str(bg_obj.get("bottom_color", "#c8ced6"))
    floor_tint = str(bg_obj.get("floor_tint", "#d5dbe2"))
    floor_tint_intensity = clamp(float(bg_obj.get("floor_tint_intensity", 0.42)), 0.0, 1.0)

    return {
        "style": style,
        "top_color": top_color,
        "bottom_color": bottom_color,
        "floor_tint": floor_tint,
        "floor_tint_intensity": floor_tint_intensity,
    }


def parse_presets(job: dict) -> dict:
    scene_cfg = job.get("scene", {}) if isinstance(job.get("scene"), dict) else {}
    camera_preset = str(scene_cfg.get("camera_preset", "phase3_three_view_realistic")).lower().strip()
    lighting_preset = str(scene_cfg.get("lighting_preset", "premium_softbox")).lower().strip()

    if camera_preset in {"phase2_three_view", "legacy"}:
        camera_preset = "phase2_three_view"
    elif camera_preset not in {"phase3_three_view_realistic", "product_studio_balanced"}:
        camera_preset = "phase3_three_view_realistic"

    if lighting_preset not in {"premium_softbox", "balanced_catalog", "high_contrast"}:
        lighting_preset = "premium_softbox"

    return {"camera_preset": camera_preset, "lighting_preset": lighting_preset}


def build_box_material_with_decal(bpy, job: dict, label_path: Path, box_obj):
    # base cardboard
    box_material = job.get("box", {}).get("material", {})
    base_color = hex_to_rgba(box_material.get("base_color", "#d8c7a6"))
    roughness = float(box_material.get("roughness", 0.7))

    label_cfg = job.get("label", {})

    image = bpy.data.images.load(str(label_path))
    if not image or image.size[0] <= 0 or image.size[1] <= 0:
        raise ValueError(f"Invalid label image: {label_path}")
    image_aspect = float(image.size[0]) / float(image.size[1])

    params = compute_print_area_params(label_cfg, image_aspect)

    width, height, depth = get_dimensions_m(job)

    material = bpy.data.materials.new(name="CardboardLabelMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output_node = nodes.new(type="ShaderNodeOutputMaterial")
    output_node.location = (1100, 0)

    base_bsdf = nodes.new(type="ShaderNodeBsdfPrincipled")
    base_bsdf.location = (650, 180)
    base_bsdf.inputs["Base Color"].default_value = base_color
    base_bsdf.inputs["Roughness"].default_value = clamp(roughness, 0.0, 1.0)
    base_bsdf.inputs["Specular IOR Level"].default_value = 0.18

    label_bsdf = nodes.new(type="ShaderNodeBsdfPrincipled")
    label_bsdf.location = (650, -100)
    label_bsdf.inputs["Roughness"].default_value = 0.28
    label_bsdf.inputs["Specular IOR Level"].default_value = 0.25

    mix_shader = nodes.new(type="ShaderNodeMixShader")
    mix_shader.location = (900, 40)

    tex_coord = nodes.new(type="ShaderNodeTexCoord")
    tex_coord.location = (-950, -40)
    tex_coord.object = box_obj

    separate = nodes.new(type="ShaderNodeSeparateXYZ")
    separate.location = (-760, -40)

    # Build panel UV from object coordinates
    # front: U=(x/w)+0.5, V=(z/h)
    # side(+X): U=(-y/d)+0.5, V=(z/h)
    # top(+Z): U=(x/w)+0.5, V=(y/d)+0.5
    # Implement with math nodes for compatibility.
    x_div = nodes.new(type="ShaderNodeMath")
    x_div.operation = "DIVIDE"
    x_div.location = (-560, 220)
    x_div.inputs[1].default_value = width

    y_div = nodes.new(type="ShaderNodeMath")
    y_div.operation = "DIVIDE"
    y_div.location = (-560, 80)
    y_div.inputs[1].default_value = depth

    z_div = nodes.new(type="ShaderNodeMath")
    z_div.operation = "DIVIDE"
    z_div.location = (-560, -60)
    z_div.inputs[1].default_value = height

    add_half_u = nodes.new(type="ShaderNodeMath")
    add_half_u.operation = "ADD"
    add_half_u.location = (-380, 220)
    add_half_u.inputs[1].default_value = 0.5

    sub_half_u = nodes.new(type="ShaderNodeMath")
    sub_half_u.operation = "SUBTRACT"
    sub_half_u.location = (-380, 80)
    sub_half_u.inputs[0].default_value = 0.5

    add_half_v_top = nodes.new(type="ShaderNodeMath")
    add_half_v_top.operation = "ADD"
    add_half_v_top.location = (-380, -60)
    add_half_v_top.inputs[1].default_value = 0.5

    combine_uv = nodes.new(type="ShaderNodeCombineXYZ")
    combine_uv.location = (-40, 20)

    # position mask for panel selection
    panel_mask = nodes.new(type="ShaderNodeMath")
    panel_mask.operation = "GREATER_THAN"
    panel_mask.location = (-40, 220)

    # front y > 0, side x > 0, top z > 0
    if params["panel"] == "side":
        panel_axis = "X"
    elif params["panel"] == "top":
        panel_axis = "Z"
    else:
        panel_axis = "Y"

    # Transform UV to requested print area (phase 3)
    mapping = nodes.new(type="ShaderNodeMapping")
    mapping.location = (130, 20)
    mapping.inputs["Location"].default_value[0] = params["content_x"]
    mapping.inputs["Location"].default_value[1] = params["content_y"]
    mapping.inputs["Scale"].default_value[0] = params["content_w"]
    mapping.inputs["Scale"].default_value[1] = params["content_h"]
    mapping.inputs["Rotation"].default_value[2] = math.radians(params["rotation_deg"])

    label_tex = nodes.new(type="ShaderNodeTexImage")
    label_tex.location = (350, 20)
    label_tex.image = image
    label_tex.interpolation = "Cubic"
    label_tex.extension = "CLIP"

    # hard rectangle mask from mapped UV inside [0..1]
    sep_mapped = nodes.new(type="ShaderNodeSeparateXYZ")
    sep_mapped.location = (350, -220)

    u_ge_0 = nodes.new(type="ShaderNodeMath")
    u_ge_0.operation = "GREATER_THAN"
    u_ge_0.location = (550, -320)

    u_le_1 = nodes.new(type="ShaderNodeMath")
    u_le_1.operation = "LESS_THAN"
    u_le_1.location = (550, -270)
    u_le_1.inputs[1].default_value = 1.0

    v_ge_0 = nodes.new(type="ShaderNodeMath")
    v_ge_0.operation = "GREATER_THAN"
    v_ge_0.location = (550, -220)

    v_le_1 = nodes.new(type="ShaderNodeMath")
    v_le_1.operation = "LESS_THAN"
    v_le_1.location = (550, -170)
    v_le_1.inputs[1].default_value = 1.0

    uv_and_1 = nodes.new(type="ShaderNodeMath")
    uv_and_1.operation = "MULTIPLY"
    uv_and_1.location = (740, -290)

    uv_and_2 = nodes.new(type="ShaderNodeMath")
    uv_and_2.operation = "MULTIPLY"
    uv_and_2.location = (740, -210)

    uv_rect_mask = nodes.new(type="ShaderNodeMath")
    uv_rect_mask.operation = "MULTIPLY"
    uv_rect_mask.location = (900, -250)

    alpha_mul = nodes.new(type="ShaderNodeMath")
    alpha_mul.operation = "MULTIPLY"
    alpha_mul.location = (900, -60)

    final_mask = nodes.new(type="ShaderNodeMath")
    final_mask.operation = "MULTIPLY"
    final_mask.location = (900, 40)

    # Slight paper texture influence on label roughness for realism
    noise = nodes.new(type="ShaderNodeTexNoise")
    noise.location = (350, 220)
    noise.inputs["Scale"].default_value = 180.0
    noise.inputs["Detail"].default_value = 2.0

    rough_mul = nodes.new(type="ShaderNodeMath")
    rough_mul.operation = "MULTIPLY"
    rough_mul.location = (550, 220)
    rough_mul.inputs[1].default_value = 0.08

    rough_add = nodes.new(type="ShaderNodeMath")
    rough_add.operation = "ADD"
    rough_add.location = (740, 220)
    rough_add.inputs[1].default_value = 0.24

    # wiring
    links.new(tex_coord.outputs["Object"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], x_div.inputs[0])
    links.new(separate.outputs["Y"], y_div.inputs[0])
    links.new(separate.outputs["Z"], z_div.inputs[0])

    links.new(x_div.outputs[0], add_half_u.inputs[0])
    links.new(y_div.outputs[0], sub_half_u.inputs[1])
    links.new(y_div.outputs[0], add_half_v_top.inputs[0])

    if params["panel"] == "side":
        links.new(sub_half_u.outputs[0], combine_uv.inputs["X"])
        links.new(z_div.outputs[0], combine_uv.inputs["Y"])
        links.new(separate.outputs[panel_axis], panel_mask.inputs[0])
    elif params["panel"] == "top":
        links.new(add_half_u.outputs[0], combine_uv.inputs["X"])
        links.new(add_half_v_top.outputs[0], combine_uv.inputs["Y"])
        links.new(separate.outputs[panel_axis], panel_mask.inputs[0])
    else:
        links.new(add_half_u.outputs[0], combine_uv.inputs["X"])
        links.new(z_div.outputs[0], combine_uv.inputs["Y"])
        links.new(separate.outputs[panel_axis], panel_mask.inputs[0])

    panel_mask.inputs[1].default_value = 0.0

    links.new(combine_uv.outputs["Vector"], mapping.inputs["Vector"])
    links.new(mapping.outputs["Vector"], label_tex.inputs["Vector"])
    links.new(mapping.outputs["Vector"], sep_mapped.inputs["Vector"])

    links.new(sep_mapped.outputs["X"], u_ge_0.inputs[0])
    links.new(sep_mapped.outputs["X"], u_le_1.inputs[0])
    links.new(sep_mapped.outputs["Y"], v_ge_0.inputs[0])
    links.new(sep_mapped.outputs["Y"], v_le_1.inputs[0])

    links.new(u_ge_0.outputs[0], uv_and_1.inputs[0])
    links.new(u_le_1.outputs[0], uv_and_1.inputs[1])
    links.new(v_ge_0.outputs[0], uv_and_2.inputs[0])
    links.new(v_le_1.outputs[0], uv_and_2.inputs[1])

    links.new(uv_and_1.outputs[0], uv_rect_mask.inputs[0])
    links.new(uv_and_2.outputs[0], uv_rect_mask.inputs[1])

    if "Alpha" in label_tex.outputs:
        links.new(label_tex.outputs["Alpha"], alpha_mul.inputs[0])
    else:
        alpha_mul.inputs[0].default_value = 1.0
    links.new(uv_rect_mask.outputs[0], alpha_mul.inputs[1])

    links.new(alpha_mul.outputs[0], final_mask.inputs[0])
    links.new(panel_mask.outputs[0], final_mask.inputs[1])

    links.new(label_tex.outputs["Color"], label_bsdf.inputs["Base Color"])
    links.new(noise.outputs["Fac"], rough_mul.inputs[0])
    links.new(rough_mul.outputs[0], rough_add.inputs[0])
    links.new(rough_add.outputs[0], label_bsdf.inputs["Roughness"])

    links.new(base_bsdf.outputs["BSDF"], mix_shader.inputs[1])
    links.new(label_bsdf.outputs["BSDF"], mix_shader.inputs[2])
    links.new(final_mask.outputs[0], mix_shader.inputs[0])

    links.new(mix_shader.outputs["Shader"], output_node.inputs["Surface"])

    set_enum_if_supported(material, "blend_method", "BLEND")
    set_enum_if_supported(material, "surface_render_method", "DITHERED")
    set_enum_if_supported(material, "shadow_method", "CLIP")

    return material


def create_box_with_label_decal(bpy, job: dict, label_path: Path):
    width, height, depth = get_dimensions_m(job)

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, height / 2.0))
    box_obj = bpy.context.object
    box_obj.name = "MockupBox"
    box_obj.scale = (width / 2.0, depth / 2.0, height / 2.0)

    material = build_box_material_with_decal(bpy, job, label_path, box_obj)
    box_obj.data.materials.clear()
    box_obj.data.materials.append(material)

    # Rounded edge bevel for realism
    bevel = box_obj.modifiers.new("Bevel", "BEVEL")
    bevel.width = min(width, depth, height) * 0.008
    bevel.segments = 3
    bevel.limit_method = "NONE"

    return box_obj, (width, height, depth)


def setup_background_world(bpy, bg_cfg: dict) -> None:
    world = bpy.data.worlds.get("World")
    if not world:
        return

    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()

    output = nodes.new(type="ShaderNodeOutputWorld")
    output.location = (320, 0)

    background = nodes.new(type="ShaderNodeBackground")
    background.location = (100, 0)

    bg_top = hex_to_rgba(bg_cfg["top_color"])
    bg_bottom = hex_to_rgba(bg_cfg["bottom_color"])

    if bg_cfg["style"] == "flat":
        background.inputs[0].default_value = bg_top
        background.inputs[1].default_value = 0.42
        links.new(background.outputs["Background"], output.inputs["Surface"])
        return

    tex_coord = nodes.new(type="ShaderNodeTexCoord")
    tex_coord.location = (-720, 0)

    mapping = nodes.new(type="ShaderNodeMapping")
    mapping.location = (-540, 0)
    mapping.inputs["Rotation"].default_value[0] = math.radians(90.0)

    gradient = nodes.new(type="ShaderNodeTexGradient")
    gradient.location = (-350, 0)
    gradient.gradient_type = "LINEAR"

    smooth = nodes.new(type="ShaderNodeMapRange")
    smooth.location = (-170, 0)
    smooth.inputs["From Min"].default_value = 0.2
    smooth.inputs["From Max"].default_value = 0.88
    smooth.inputs["To Min"].default_value = 0.0
    smooth.inputs["To Max"].default_value = 1.0
    smooth.clamp = True

    ramp = nodes.new(type="ShaderNodeValToRGB")
    ramp.location = (-10, 0)
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = bg_bottom
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = bg_top

    background.inputs[1].default_value = 0.55 if bg_cfg["style"] == "auto_contrast_studio" else 0.48

    links.new(tex_coord.outputs["Generated"], mapping.inputs["Vector"])
    links.new(mapping.outputs["Vector"], gradient.inputs["Vector"])
    links.new(gradient.outputs["Fac"], smooth.inputs["Value"])
    links.new(smooth.outputs["Result"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], background.inputs["Color"])
    links.new(background.outputs["Background"], output.inputs["Surface"])


def setup_lighting_and_world(bpy, dims: tuple[float, float, float], job: dict) -> None:
    width, height, depth = dims

    bg_cfg = parse_scene_background(job)
    presets = parse_presets(job)

    setup_background_world(bpy, bg_cfg)

    floor_mix = clamp(bg_cfg["floor_tint_intensity"], 0.0, 1.0)
    floor_tint = hex_to_rgba(bg_cfg["floor_tint"])
    floor_base = lerp_color((0.92, 0.92, 0.92, 1.0), floor_tint, floor_mix)

    # Ground plane for contact realism
    bpy.ops.mesh.primitive_plane_add(size=6.5, location=(0.0, 0.0, 0.0))
    ground = bpy.context.object
    ground.name = "Ground"
    gmat = bpy.data.materials.new(name="GroundMaterial")
    gmat.use_nodes = True
    gbsdf = gmat.node_tree.nodes.get("Principled BSDF")
    gbsdf.inputs["Base Color"].default_value = floor_base
    gbsdf.inputs["Roughness"].default_value = 0.78
    gbsdf.inputs["Specular IOR Level"].default_value = 0.08
    ground.data.materials.append(gmat)

    # Back wall for clean studio two-tone read and color separation
    bpy.ops.mesh.primitive_plane_add(size=6.5, location=(0.0, depth * 1.9, height * 1.3))
    wall = bpy.context.object
    wall.name = "BackdropWall"
    wall.rotation_euler = (math.radians(90.0), 0.0, 0.0)

    wmat = bpy.data.materials.new(name="BackdropWallMaterial")
    wmat.use_nodes = True
    wnodes = wmat.node_tree.nodes
    wlinks = wmat.node_tree.links

    wall_bsdf = wnodes.get("Principled BSDF")
    wall_output = wnodes.get("Material Output")
    if wall_bsdf and wall_output:
        wall_bsdf.inputs["Roughness"].default_value = 0.93
        wall_bsdf.inputs["Specular IOR Level"].default_value = 0.0
        wall_bsdf.inputs["Base Color"].default_value = lerp_color(
            hex_to_rgba(bg_cfg["bottom_color"]),
            hex_to_rgba(bg_cfg["top_color"]),
            0.55,
        )

        if bg_cfg["style"] in {"auto_contrast_studio", "dual_tone"}:
            tex_coord = wnodes.new(type="ShaderNodeTexCoord")
            tex_coord.location = (-560, 40)
            sep = wnodes.new(type="ShaderNodeSeparateXYZ")
            sep.location = (-360, 40)
            ramp = wnodes.new(type="ShaderNodeValToRGB")
            ramp.location = (-170, 40)
            ramp.color_ramp.elements[0].position = 0.18
            ramp.color_ramp.elements[0].color = hex_to_rgba(bg_cfg["bottom_color"])
            ramp.color_ramp.elements[1].position = 0.9
            ramp.color_ramp.elements[1].color = hex_to_rgba(bg_cfg["top_color"])

            wlinks.new(tex_coord.outputs["Object"], sep.inputs["Vector"])
            wlinks.new(sep.outputs["Z"], ramp.inputs["Fac"])
            wlinks.new(ramp.outputs["Color"], wall_bsdf.inputs["Base Color"])

    wall.data.materials.append(wmat)

    if presets["lighting_preset"] == "balanced_catalog":
        key_energy = 980
        fill_energy = 520
        rim_energy = 420
        top_energy = 180
    elif presets["lighting_preset"] == "high_contrast":
        key_energy = 1380
        fill_energy = 360
        rim_energy = 620
        top_energy = 140
    else:  # premium_softbox default
        key_energy = 1200
        fill_energy = 560
        rim_energy = 500
        top_energy = 220

    # Key light
    bpy.ops.object.light_add(type="AREA", location=(width * 2.45, -(depth * 2.75), height * 2.2))
    key = bpy.context.object
    key.name = "KeyLight"
    key.data.energy = key_energy
    key.data.size = 1.7

    # Fill light
    bpy.ops.object.light_add(type="AREA", location=(-(width * 2.65), -(depth * 1.55), height * 1.55))
    fill = bpy.context.object
    fill.name = "FillLight"
    fill.data.energy = fill_energy
    fill.data.size = 2.2

    # Rim/back light for separation on all object colors
    bpy.ops.object.light_add(type="AREA", location=(0.0, depth * 2.6, height * 1.9))
    rim = bpy.context.object
    rim.name = "RimLight"
    rim.data.energy = rim_energy
    rim.data.size = 1.1

    # Gentle top light to keep white boxes readable without flattening black boxes
    bpy.ops.object.light_add(type="AREA", location=(0.0, -(depth * 0.4), height * 3.1))
    top = bpy.context.object
    top.name = "TopLight"
    top.data.energy = top_energy
    top.data.size = 2.8


def add_camera(bpy, name: str, location: tuple[float, float, float], rotation: tuple[float, float, float], lens: float):
    bpy.ops.object.camera_add(location=location, rotation=rotation)
    cam = bpy.context.object
    cam.name = name
    cam.data.lens = lens
    return cam


def setup_cameras(bpy, dims: tuple[float, float, float], job: dict):
    width, height, depth = dims
    preset = parse_presets(job)["camera_preset"]

    if preset == "phase2_three_view":
        return {
            "front": add_camera(
                bpy,
                "CamFront",
                (0.0, -(depth * 3.0 + 0.28), height * 0.60),
                (1.535, 0.0, 0.0),
                70.0,
            ),
            "angle": add_camera(
                bpy,
                "CamAngle",
                (width * 2.15 + 0.24, -(depth * 2.35 + 0.20), height * 0.80),
                (1.16, 0.0, 0.94),
                58.0,
            ),
            "closeup": add_camera(
                bpy,
                "CamCloseup",
                (0.0, -(depth * 1.35 + 0.08), height * 0.58),
                (1.54, 0.0, 0.0),
                85.0,
            ),
        }

    if preset == "product_studio_balanced":
        return {
            "front": add_camera(
                bpy,
                "CamFront",
                (0.0, -(depth * 2.95 + 0.30), height * 0.62),
                (1.525, 0.0, 0.0),
                74.0,
            ),
            "angle": add_camera(
                bpy,
                "CamAngle",
                (width * 2.35 + 0.26, -(depth * 2.15 + 0.24), height * 0.82),
                (1.13, 0.0, 0.92),
                62.0,
            ),
            "closeup": add_camera(
                bpy,
                "CamCloseup",
                (0.0, -(depth * 1.25 + 0.09), height * 0.62),
                (1.535, 0.0, 0.0),
                90.0,
            ),
        }

    # phase3_three_view_realistic default
    return {
        "front": add_camera(
            bpy,
            "CamFront",
            (0.0, -(depth * 3.1 + 0.30), height * 0.62),
            (1.525, 0.0, 0.0),
            72.0,
        ),
        "angle": add_camera(
            bpy,
            "CamAngle",
            (width * 2.42 + 0.28, -(depth * 2.05 + 0.25), height * 0.84),
            (1.11, 0.0, 0.905),
            60.0,
        ),
        "closeup": add_camera(
            bpy,
            "CamCloseup",
            (0.0, -(depth * 1.20 + 0.10), height * 0.64),
            (1.53, 0.0, 0.0),
            92.0,
        ),
    }


def configure_render_settings(bpy, job: dict):
    scene = bpy.context.scene
    output = job.get("output", {})
    res = output.get("resolution", {})

    scene.render.engine = "BLENDER_EEVEE"
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.resolution_x = int(res.get("width", 1280))
    scene.render.resolution_y = int(res.get("height", 720))

    # Better defaults for realism
    eevee = scene.eevee
    if hasattr(eevee, "taa_render_samples"):
        eevee.taa_render_samples = 96
    if hasattr(eevee, "use_gtao"):
        eevee.use_gtao = True
    if hasattr(eevee, "gtao_distance"):
        eevee.gtao_distance = 0.2
    if hasattr(eevee, "use_bloom"):
        eevee.use_bloom = False


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
    _, dims = create_box_with_label_decal(bpy, job, label_path)
    setup_lighting_and_world(bpy, dims, job)
    cams = setup_cameras(bpy, dims, job)
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
