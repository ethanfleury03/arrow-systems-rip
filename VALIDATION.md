# Validation Notes: Adaptive Print Wait Logic

## What changed

- `--timeout <sec>` still works and now **forces** the hard timeout explicitly.
- If `--timeout` is omitted, the RIP computes a dynamic hard timeout using:
  - input file size (MB)
  - requested/effective page count
  - DPI
  - color mode (mono vs CMYK)
- Verification wait is now completion-driven from Gymea job signals, not fixed-sleep driven.
- Added stall detection: if no new progress signals appear for too long, verification fails with a clear reason.

## Adaptive timeout model

```text
hard_timeout_sec = clamp(
  (base + per_page*pages + per_mb*file_mb) * dpi_multiplier * color_multiplier,
  min=35,
  max=420
)

base=18, per_page=14, per_mb=0.55

dpi_multiplier:
  <=600:1.0, <=1200:1.2, <=1600:1.45, <=2400:1.8, >2400:2.0

color_multiplier:
  mono=1.0, color=1.35
```

Stall timeout defaults to `max(20, hard_timeout/3)` capped at 120s.

## Useful env overrides (optional)

- `RIP_VERIFY_POLL_MS` (default `1000`)
- `RIP_VERIFY_STALL_SEC` (default derived from hard timeout)

## Windows manual verification commands

Run from `C:\Users\Arrow\Arrow-Rip`:

```bat
REM 1) Build (if needed)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

REM 2) Adaptive timeout path (no --timeout)
set USE_LEGACY_ORCHESTRATION=0
build\Release\memjet-rip.exe -i C:\print\sample.pdf --pes-ip 192.168.1.100 --cmyk -v

REM 3) Explicit timeout override path (CLI compatibility)
build\Release\memjet-rip.exe -i C:\print\sample.pdf --pes-ip 192.168.1.100 --timeout 90 -v

REM 4) Stall detection check (force a short stall threshold)
set RIP_VERIFY_STALL_SEC=10
build\Release\memjet-rip.exe -i C:\print\sample.pdf --pes-ip 192.168.1.100 -v

REM 5) Dry-run sanity (should not hit print verification path)
build\Release\memjet-rip.exe -i C:\print\sample.pdf --dry-run -v
```

Expected behavior:
- Command logs either explicit timeout or adaptive timeout details.
- Verification state logs include transitions like queued/sending/printing/completed/fault when observed.
- Completion succeeds on required Gymea completion signals.
- Timeout now acts as guardrail; failures indicate explicit reason (stall/fault/cancel/hard-timeout).

## Default color behavior + mono override validation (Ethan)

Run from `C:\Users\Arrow\Arrow-Rip`:

```bat
REM 1) DEFAULT path (CMYK color, no color flag required)
set USE_FAST_MONO=0
set USE_TRUE_CMYK=1
build\Release\memjet-rip.exe -i C:\print\ethan-problem.pdf --dry-run --dpi 1600 -v

REM 2) Explicit mono override via CLI
build\Release\memjet-rip.exe -i C:\print\ethan-problem.pdf --dry-run --dpi 1600 --gray -v

REM 3) Explicit mono override via env
set USE_FAST_MONO=1
build\Release\memjet-rip.exe -i C:\print\ethan-problem.pdf --dry-run --dpi 1600 -v
```

Expected behavior:
- Default run logs `Render mode selected: CMYK_COLOR` and `Plane processing plan: 4 plane(s) [Magenta, Cyan, Black, Yellow]`.
- Mono only happens when explicitly requested (`--gray/--mono` or `USE_FAST_MONO=1`).
- RIP stays at 1600 DPI (no automatic fallback to lower DPI).
- CMYK Ghostscript path is fail-fast: if GS exit is non-zero OR stderr includes draw-failure signatures (`Page drawing error occurred`, `Could not draw this page at all`, `page will be missing in the output`), RIP aborts before PAM parsing/plane split.
- CMYK PAM output is validated before use: header + payload size check (`expectedTotalBytes` vs `actualBytes`, plus expected payload bytes).
- One guarded retry at the same 1600 DPI is allowed only for post-GS CMYK PAM validation/read failures (fresh temp output path each attempt).

## CMYK 1600 fail-fast regression command (Ethan)

Run from `C:\Users\Arrow\Arrow-Rip` with Ethan's known repro PDF:

```bat
set USE_FAST_MONO=0
set USE_TRUE_CMYK=1
build\Release\memjet-rip.exe -i C:\print\ethan-problem.pdf --pes-ip 192.168.1.100 --dpi 1600 --cmyk -v
```

Expected CMYK 1600 diagnostics:
- Logs include CMYK GS `attempt`, `exitCode`, stderr tail, output file size, and expected payload size.
- On draw-failure signature, command aborts immediately with actionable GS error (no `Incomplete CMYK PAM payload`).
- On truncated PAM race, one retry is attempted at same DPI with a fresh temp `.pam` path, then hard-fails with precise expected vs actual byte counts.
