#!/usr/bin/env python3
"""
Prepare/validate assets for the Blender box mockup MVP.

This script can run in normal Python (no Blender dependency).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve()
DEFAULT_REPO_ROOT = SCRIPT_PATH.parents[2]


REQUIRED_TOP_KEYS = ["job_id", "scene", "box", "label", "output"]
REQUIRED_VIEW_KEYS = ["front", "angle", "closeup"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare assets for box mockup render jobs")
    parser.add_argument("--job", required=True, help="Path to job JSON")
    parser.add_argument("--create-dirs", action="store_true", help="Create missing output dirs")
    return parser.parse_args()


def _is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


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


def validate_job(job: dict) -> list[str]:
    errors: list[str] = []
    missing = [k for k in REQUIRED_TOP_KEYS if k not in job]
    if missing:
        errors.append(f"Job file missing required keys: {', '.join(missing)}")
        return errors

    dims = job.get("box", {}).get("dimensions_mm", {})
    for k in ("width", "height", "depth"):
        v = dims.get(k)
        if not _is_number(v) or float(v) <= 0:
            errors.append(f"box.dimensions_mm.{k} must be a positive number")

    label = job.get("label", {})
    placement = label.get("placement", {})
    scale = label.get("scale", {})

    for k in ("center_x", "center_y"):
        if k in placement:
            v = placement[k]
            if not _is_number(v) or not (0.0 <= float(v) <= 1.0):
                errors.append(f"label.placement.{k} must be a number in [0, 1]")

    for k in ("width", "height"):
        if k in scale:
            v = scale[k]
            if not _is_number(v) or not (0.05 <= float(v) <= 1.0):
                errors.append(f"label.scale.{k} must be a number in [0.05, 1.0]")

    output = job.get("output", {})
    views = output.get("views")
    if views is not None:
        if not isinstance(views, dict):
            errors.append("output.views must be an object when provided")
        else:
            missing_views = [k for k in REQUIRED_VIEW_KEYS if k not in views]
            if missing_views:
                errors.append(f"output.views missing required keys: {', '.join(missing_views)}")

    return errors


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


def resolve_output_paths(job: dict, job_path: Path) -> dict[str, Path]:
    output = job.get("output", {})
    views = output.get("views")
    if isinstance(views, dict) and views:
        return {k: absolutize_output_path(Path(v), job_path) for k, v in views.items() if k in REQUIRED_VIEW_KEYS}

    base = absolutize_output_path(Path(output.get("image_path", "renders/box-mockup-mvp-001.png")), job_path)
    stem = base.stem
    suffix = base.suffix or ".png"
    return {
        "front": base.parent / f"{stem}-front{suffix}",
        "angle": base.parent / f"{stem}-angle{suffix}",
        "closeup": base.parent / f"{stem}-closeup{suffix}",
    }


def main() -> int:
    args = parse_args()
    job_path = Path(args.job).expanduser()
    if not job_path.is_absolute():
        job_path = (Path.cwd() / job_path).absolute()

    if not job_path.exists():
        print(f"[error] Job file does not exist: {job_path}")
        return 1

    with job_path.open("r", encoding="utf-8") as f:
        job = json.load(f)

    errors = validate_job(job)
    if errors:
        for e in errors:
            print(f"[error] {e}")
        return 1

    label_path = resolve_input_path(str(job.get("label", {}).get("image_path", "")), job_path)
    if not label_path.exists():
        print(f"[warn] Label image missing: {label_path}")
    else:
        print(f"[ok] Label image found: {label_path}")

    output_paths = resolve_output_paths(job, job_path)
    if args.create_dirs:
        created = set()
        for out in output_paths.values():
            out.parent.mkdir(parents=True, exist_ok=True)
            created.add(str(out.parent))
        for p in sorted(created):
            print(f"[ok] Ensured output dir: {p}")
    else:
        missing = sorted({str(p.parent) for p in output_paths.values() if not p.parent.exists()})
        for p in missing:
            print(f"[warn] Output dir does not exist: {p} (use --create-dirs)")

    print("[ok] Asset preparation step complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
