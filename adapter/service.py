from __future__ import annotations

import json
import os
import subprocess
import threading
import uuid
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field, field_validator


MAX_LOG_TAIL = 200
DEFAULT_RIP_COMMAND = ["./src/build/memjet-rip"]


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


class JobRequest(BaseModel):
    input_path: str = Field(..., min_length=1)
    args: List[str] = Field(default_factory=list)
    env: Dict[str, str] = Field(default_factory=dict)

    @field_validator("input_path")
    @classmethod
    def _path_exists(cls, value: str) -> str:
        if not Path(value).exists():
            raise ValueError(f"input_path does not exist: {value}")
        return value


class JobState(BaseModel):
    id: str
    status: str
    created_at: str
    updated_at: str
    payload: Dict[str, Any]
    events: List[Dict[str, Any]]
    logs_tail: List[str]
    exit_code: Optional[int] = None
    error_code: Optional[str] = None


app = FastAPI(title="RIP Adapter Service", version="0.1.0")
_jobs: Dict[str, Dict[str, Any]] = {}
_lock = threading.Lock()


def _default_command() -> List[str]:
    raw = os.getenv("RIP_COMMAND")
    if raw:
        return raw.split()
    return DEFAULT_RIP_COMMAND


def _append_log(job: Dict[str, Any], line: str) -> None:
    logs = job.setdefault("logs_tail", deque(maxlen=MAX_LOG_TAIL))
    logs.append(line.rstrip("\n"))


def _transition(job: Dict[str, Any], status: str, event: Optional[Dict[str, Any]] = None) -> None:
    job["status"] = status
    job["updated_at"] = _utc_now()
    if event:
        job.setdefault("events", []).append(event)


def _consume_line(job: Dict[str, Any], line: str) -> None:
    _append_log(job, line)
    stripped = line.strip()
    if not stripped.startswith("{"):
        return
    try:
        payload = json.loads(stripped)
    except json.JSONDecodeError:
        return

    job.setdefault("events", []).append(payload)
    event = payload.get("event")
    if event == "rip.job.created":
        _transition(job, "preparing")
    elif event == "rip.completed":
        _transition(job, "completed")
    elif event == "rip.failed":
        _transition(job, "failed")
    if payload.get("error_code"):
        job["error_code"] = str(payload["error_code"])


def _run_job(job_id: str, command: List[str], env_overrides: Dict[str, str]) -> None:
    with _lock:
        job = _jobs[job_id]
        _transition(job, "running")

    env = os.environ.copy()
    env.update(env_overrides)

    try:
        proc = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
    except Exception as exc:
        with _lock:
            job = _jobs[job_id]
            job["exit_code"] = -1
            job["error_code"] = "RIP_RUNTIME_EXCEPTION"
            _transition(job, "failed", {"event": "rip.failed", "message": str(exc), "error_code": "RIP_RUNTIME_EXCEPTION"})
        return

    assert proc.stdout is not None
    assert proc.stderr is not None

    for stream in (proc.stdout, proc.stderr):
        for line in stream:
            with _lock:
                job = _jobs[job_id]
                _consume_line(job, line)

    exit_code = proc.wait()
    with _lock:
        job = _jobs[job_id]
        job["exit_code"] = int(exit_code)
        if job["status"] not in {"completed", "failed"}:
            if exit_code == 0:
                _transition(job, "completed", {"event": "rip.completed"})
            else:
                job["error_code"] = job.get("error_code") or "RIP_RUNTIME_EXCEPTION"
                _transition(job, "failed", {"event": "rip.failed", "error_code": job["error_code"]})


def start_job(job_id: str, payload: JobRequest) -> List[str]:
    command = _default_command() + [payload.input_path] + payload.args
    thread = threading.Thread(target=_run_job, args=(job_id, command, payload.env), daemon=True)
    thread.start()
    return command


@app.get("/health")
def health() -> Dict[str, Any]:
    return {"ok": True, "service": "rip-adapter", "time": _utc_now()}


@app.post("/jobs", status_code=202)
def submit_job(request: JobRequest) -> Dict[str, Any]:
    job_id = str(uuid.uuid4())
    now = _utc_now()
    entry = {
        "id": job_id,
        "status": "queued",
        "created_at": now,
        "updated_at": now,
        "payload": request.model_dump(),
        "events": [],
        "logs_tail": deque(maxlen=MAX_LOG_TAIL),
        "exit_code": None,
        "error_code": None,
    }
    with _lock:
        _jobs[job_id] = entry

    try:
        command = start_job(job_id, request)
    except Exception as exc:
        with _lock:
            job = _jobs[job_id]
            job["status"] = "failed"
            job["updated_at"] = _utc_now()
            job["error_code"] = "RIP_RUNTIME_EXCEPTION"
            job["events"].append({"event": "rip.failed", "error_code": "RIP_RUNTIME_EXCEPTION", "message": str(exc)})
        raise HTTPException(status_code=500, detail="Failed to launch RIP process") from exc

    return {"id": job_id, "status": "queued", "command": command}


@app.get("/jobs/{job_id}")
def get_job(job_id: str) -> JobState:
    with _lock:
        job = _jobs.get(job_id)
        if not job:
            raise HTTPException(status_code=404, detail="job not found")
        copy = dict(job)
        copy["logs_tail"] = list(copy.get("logs_tail", []))
    return JobState(**copy)
