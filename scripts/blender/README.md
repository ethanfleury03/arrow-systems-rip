# Blender Box Mockup Pipeline

## Phase 3.1 art direction (color-agnostic studio)

Renderer now defaults to a **neutral mid-gray background** (when `scene.safe_mode=true`) so white, black, and saturated product colors all stay readable without harsh diagonals.

### New scene fields

```json
{
  "scene": {
    "safe_mode": true,
    "background": {
      "style": "neutral_midgray",
      "top_color": "#8c8f96",
      "bottom_color": "#666a72",
      "floor_tint": "#777b84",
      "floor_tint_intensity": 0.20
    },
    "camera_preset": "phase3_three_view_realistic",
    "lighting_preset": "premium_softbox"
  }
}
```

#### `scene.background`
- Legacy string still supported (ex: `"studio_gray"`).
- Object form fields:
  - `style`: `neutral_midgray` (default) | `auto_contrast_studio` | `dual_tone` | `flat`
  - `top_color`: hex color (top/background high tone)
  - `bottom_color`: hex color (horizon/lower backdrop tone)
  - `floor_tint`: hex color blended into ground for contact realism
  - `floor_tint_intensity`: `0..1` blend amount

#### `scene.safe_mode`
- Optional boolean (recommended `true` for phase 3 examples).
- When enabled, forces conservative defaults:
  - neutral mid-gray world background
  - softer world strength + light energies
  - per-shot camera correction + emergency fallback orbit
  - no backdrop wall plane (avoids giant diagonal split artifacts)

#### Presets
- `scene.camera_preset`:
  - `phase3_three_view_realistic` (default)
  - `product_studio_balanced`
  - `phase2_three_view` (legacy framing)
- `scene.lighting_preset`:
  - `premium_softbox` (default)
  - `balanced_catalog`
  - `high_contrast`

Lighting rig includes key/fill/rim/top lights and a contact-shadow ground plane for consistent separation.

## Phase 3 placement model

Label is applied directly onto the box material (decal/overlay), not as a floating plane.

### `label.print_area`

```json
{
  "label": {
    "print_area": {
      "panel": "front",
      "bounds": { "x": 0.1, "y": 0.1, "width": 0.8, "height": 0.8 },
      "rotation": 0,
      "scale_mode": "fit",
      "bleed": 0.01
    }
  }
}
```

- `panel`: `front` | `side` | `top` (default `front`)
- `bounds` (normalized `0..1`) or direct `x/y/width/height`
- `rotation`: degrees
- `scale_mode`: `fit` or `fill`
- `bleed`: normalized expansion (`0..0.49`)

## Backward compatibility

Legacy Phase 2 fields still work and are auto-mapped:
- `label.target_face`
- `label.placement.center_x / center_y`
- `label.scale.width / height`

Legacy scene values still work:
- `scene.background: "studio_gray"`
- `scene.camera_preset: "phase2_three_view"`

Existing `scripts/blender/examples/box_job.phase2.example.json` remains valid.

## Examples

- Legacy: `scripts/blender/examples/box_job.phase2.example.json`
- New Phase 3.1: `scripts/blender/examples/box_job.phase3.example.json`

## Validate assets

```bash
python3 scripts/blender/prepare_assets.py --job scripts/blender/examples/box_job.phase3.example.json --create-dirs
```

## Render

```bash
./scripts/blender/run_blender_headless.sh scripts/blender/examples/box_job.phase3.example.json
```

## If render is blank (white/black frame)

Use diagnostics first:

```bash
blender -b -P scripts/blender/render_box_mockup.py -- --job scripts/blender/examples/box_job.phase3.example.json --debug
```

What to tune (in `scene`):
- `lighting.intensity_scale` (recommended range `0.7` to `1.0`)
- `background.top_color` / `background.bottom_color` (keep subtle: mid-gray range)
- `lighting_preset` (`premium_softbox` is safest default)

Copy/paste neutral-background tuning block:

```json
{
  "scene": {
    "safe_mode": true,
    "background": {
      "style": "neutral_midgray",
      "top_color": "#8c8f96",
      "bottom_color": "#666a72",
      "floor_tint": "#777b84",
      "floor_tint_intensity": 0.20
    },
    "lighting": {
      "intensity_scale": 0.9
    }
  }
}
```

The renderer computes camera placement from the object world bounding box before each shot, performs one auto-reframe correction if coverage/visibility is invalid, and finally applies a deterministic fallback orbit if still invalid, so the product stays in-frame.
