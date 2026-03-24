# Blender Box Mockup MVP (Box-only)

## Goal
Create a minimal, implementation-ready pipeline to render a **single box mockup** with a label image using **headless Blender**.

This MVP is scaffold-first: scripts validate inputs, provide a stable run path, and include placeholders where geometry/material logic will be expanded.

---

## Architecture (MVP)

- **Job spec (JSON)**
  - Source of truth for dimensions, label asset, and output path.
  - Example: `scripts/blender/examples/box_job.example.json`

- **Asset prep/validation script**
  - `scripts/blender/prepare_assets.py`
  - Runs in normal Python (no `bpy` required).
  - Checks expected files/dirs before render.

- **Headless render script (Blender Python)**
  - `scripts/blender/render_box_mockup.py`
  - Intended to run via `blender -b -P ... -- --job ...`
  - Currently includes basic scene bootstrap + render output plumbing.

- **Execution helper**
  - `scripts/blender/run_blender_headless.sh`
  - Handles Blender presence checks and clear error guidance.

---

## Directory Layout

```text
docs/
  blender-box-mockup-mvp.md

scripts/blender/
  prepare_assets.py
  render_box_mockup.py
  run_blender_headless.sh
  examples/
    box_job.example.json
```

---

## Prerequisites

1. **Blender** installed and available on PATH (`blender` command).
2. Python 3.x for prep script.
3. Label image asset (path configured in job JSON).

If Blender is missing, helper exits safely with install instructions.

---

## Setup

From repo root:

```bash
chmod +x scripts/blender/run_blender_headless.sh
python3 scripts/blender/prepare_assets.py --job scripts/blender/examples/box_job.example.json --create-dirs
```

Then put a label image at:

```text
assets/labels/example-label.png
```

(or update `label.image_path` in the job file)

---

## Runbook

### 1) Validate/prepare assets

```bash
python3 scripts/blender/prepare_assets.py --job scripts/blender/examples/box_job.example.json --create-dirs
```

### 2) Render headless

```bash
./scripts/blender/run_blender_headless.sh scripts/blender/examples/box_job.example.json
```

### 3) Expected output

Configured by `output.image_path`, default example:

```text
renders/box-mockup-mvp-001.png
```

---

## Job Schema (current MVP fields)

Top-level required keys:
- `job_id`
- `scene`
- `box`
- `label`
- `output`

Important nested fields used now:
- `label.image_path`
- `output.image_path`
- `output.resolution.width`
- `output.resolution.height`

Planned near-term extensions:
- Exact UV projection logic for front-face label fitting.
- Material presets and lighting presets.
- Deterministic camera framing by box size.

---

## Implementation Notes

- `render_box_mockup.py` fails fast with readable errors if:
  - job file is missing/invalid
  - script is run outside Blender runtime (`bpy` unavailable)
- Scene logic is intentionally minimal to keep MVP stable while assets/requirements finalize.

---

## Next Steps

1. Build actual box mesh and map dimensions from `box.dimensions_mm`.
2. Create material + texture node graph for `label.image_path`.
3. Apply label only to `label.target_face` with `fit_mode` handling.
4. Add optional transparent background output (`RGBA`) for compositing.
5. Add a simple regression test harness (golden image checksum or render metadata checks).
