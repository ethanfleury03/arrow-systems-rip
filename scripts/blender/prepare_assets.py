#!/usr/bin/env python3
"""
Prepare/validate assets for the Blender box mockup MVP.

This script can run in normal Python (no Blender dependency).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare assets for box mockup render jobs")
    parser.add_argument("--job", required=True, help="Path to job JSON")
    parser.add_argument("--create-dirs", action="store_true", help="Create missing output dirs")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    job_path = Path(args.job)

    if not job_path.exists():
        print(f"[error] Job file does not exist: {job_path}")
        return 1

    with job_path.open("r", encoding="utf-8") as f:
        job = json.load(f)

    label_path = Path(job.get("label", {}).get("image_path", ""))
    if not label_path.exists():
        print(f"[warn] Label image missing: {label_path}")
    else:
        print(f"[ok] Label image found: {label_path}")

    output_image = Path(job.get("output", {}).get("image_path", "renders/box-preview.png"))
    if args.create_dirs:
        output_image.parent.mkdir(parents=True, exist_ok=True)
        print(f"[ok] Ensured output dir: {output_image.parent}")
    else:
        if not output_image.parent.exists():
            print(f"[warn] Output dir does not exist: {output_image.parent} (use --create-dirs)")

    print("[ok] Asset preparation step complete (MVP scaffold).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
