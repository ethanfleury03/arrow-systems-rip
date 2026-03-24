# Blender Box Mockup Pipeline

## Phase 3 placement model

The renderer now applies the label directly onto the box material (decal/overlay), not as a floating plane.

### New job fields (`label.print_area`)

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
- `bounds` (normalized 0..1) or direct `x/y/width/height`
- `rotation`: degrees
- `scale_mode`: `fit` or `fill`
- `bleed`: normalized expansion (0..0.49)

### Backward compatibility

Legacy Phase 2 fields are still accepted and mapped automatically:

- `label.target_face`
- `label.placement.center_x / center_y`
- `label.scale.width / height`

Existing `scripts/blender/examples/box_job.phase2.example.json` remains valid.

## Examples

- Legacy: `scripts/blender/examples/box_job.phase2.example.json`
- New Phase 3: `scripts/blender/examples/box_job.phase3.example.json`

## Validate assets

```bash
python3 scripts/blender/prepare_assets.py --job scripts/blender/examples/box_job.phase3.example.json --create-dirs
```

## Render

```bash
./scripts/blender/run_blender_headless.sh scripts/blender/examples/box_job.phase3.example.json
```
