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

## 1600-DPI Ghostscript fail-fast validation (Ethan)

Run from `C:\Users\Arrow\Arrow-Rip`:

```bat
set USE_TRUE_CMYK=0
set USE_FAST_MONO=0
build\Release\memjet-rip.exe -i C:\print\ethan-problem.pdf --dry-run --dpi 1600 -v
```

Expected behavior:
- RIP stays at 1600 DPI (no automatic fallback to lower DPI).
- If Ghostscript exits non-zero OR stderr includes page draw failure signatures, RIP aborts immediately before bilevel conversion.
- If PGM is truncated/partial, RIP reports exact `actualBytes` vs `expectedTotalBytes` and aborts.
- One guarded retry may occur at the same 1600 DPI only when first-pass output validation fails (transient file race).
