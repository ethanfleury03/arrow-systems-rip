# Blender Box Mockup MVP (Phase 2)

## Goal
Render a **box with a real front label overlay** using **headless Blender**, and export 3 images per job:
- front
- angle
- closeup

---

## Architecture

- **Job spec (JSON)**
  - Source of truth for dimensions, label placement/scale, and output paths.
  - Backward compatible with old single `output.image_path` jobs.
  - Examples:
    - `scripts/blender/examples/box_job.example.json` (legacy-style, still works)
    - `scripts/blender/examples/box_job.phase2.example.json` (explicit 3-view output)

- **Asset prep/validation script**
  - `scripts/blender/prepare_assets.py`
  - Runs in normal Python (no `bpy` required).
  - Validates dimensions and optional new label placement keys.

- **Headless render script (Blender Python)**
  - `scripts/blender/render_box_mockup.py`
  - Creates box mesh from mm dimensions
  - Applies cardboard material
  - Adds front label plane with image texture + alpha
  - Renders front / angle / closeup outputs

- **Execution helper**
  - `scripts/blender/run_blender_headless.sh`

---

## Prerequisites

1. Blender installed and on PATH (`blender` command)
2. Python 3
3. Label image exists at `label.image_path`

If Blender is missing, helper exits with clear install instructions.
If label image is missing, prepare step warns and render step exits cleanly with an error.

---

## Run (exact commands)

From repo root:

```bash
python3 scripts/blender/prepare_assets.py --job scripts/blender/examples/box_job.phase2.example.json --create-dirs
./scripts/blender/run_blender_headless.sh scripts/blender/examples/box_job.phase2.example.json
```

---

## Expected Output Files

For `box_job.phase2.example.json`:

```text
renders/box-mockup-phase2-001-front.png
renders/box-mockup-phase2-001-angle.png
renders/box-mockup-phase2-001-closeup.png
```

For legacy jobs using only `output.image_path`, render paths are auto-derived:

```text
<stem>-front.png
<stem>-angle.png
<stem>-closeup.png
```

Example legacy input:
- `output.image_path = renders/box-mockup-mvp-001.png`

Derived outputs:
- `renders/box-mockup-mvp-001-front.png`
- `renders/box-mockup-mvp-001-angle.png`
- `renders/box-mockup-mvp-001-closeup.png`

---

## Job Schema (Phase 2)

Required top-level keys:
- `job_id`
- `scene`
- `box`
- `label`
- `output`

Used fields:
- `box.dimensions_mm.width|height|depth` (positive numbers)
- `box.material.base_color` (hex, optional)
- `box.material.roughness` (0..1, optional)
- `label.image_path`
- `label.placement.center_x` (optional, default `0.5`, range `[0..1]`)
- `label.placement.center_y` (optional, default `0.5`, range `[0..1]`)
- `label.scale.width` (optional, default `0.8`, range `[0.05..1]`)
- `label.scale.height` (optional, default `0.8`, range `[0.05..1]`)
- `output.resolution.width|height` (optional, default `1280x720`)
- `output.views.front|angle|closeup` (optional set of explicit output paths)
- `output.image_path` (legacy fallback; used to derive all 3 view paths)

---

## Notes

- Run `prepare_assets.py` before Blender render to catch schema/path issues quickly.
- Render script only works inside Blender Python runtime (`bpy`).
- Label overlay is currently implemented as a front-face plane (fast and predictable for MVP).
