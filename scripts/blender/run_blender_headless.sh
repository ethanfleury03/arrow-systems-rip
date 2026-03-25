#!/usr/bin/env bash
set -euo pipefail

JOB_FILE="${1:-scripts/blender/examples/box_job.example.json}"
SCRIPT_PATH="scripts/blender/render_box_mockup.py"

if ! command -v blender >/dev/null 2>&1; then
  echo "[error] Blender is not installed or not in PATH."
  echo "Install Blender from https://www.blender.org/download/"
  echo "Then re-run: $0 <job-file.json>"
  exit 1
fi

if [ ! -f "$JOB_FILE" ]; then
  echo "[error] Job file not found: $JOB_FILE"
  echo "Try: $0 scripts/blender/examples/box_job.example.json"
  exit 1
fi

if [ ! -f "$SCRIPT_PATH" ]; then
  echo "[error] Render script not found: $SCRIPT_PATH"
  exit 1
fi

echo "[info] Running Blender headless with job: $JOB_FILE"
blender -b -P "$SCRIPT_PATH" -- --job "$JOB_FILE"
