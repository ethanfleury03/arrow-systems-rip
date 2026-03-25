# REQUIRED_RUNTIME

This repo is structured so `git pull` + two commands should be enough:

1. `scripts\rebuild.bat`
2. `scripts\test-print.bat "C:\path\job.pdf"`

## Required runtime dependencies

- Ghostscript CLI in `PATH` (`gswin64c`)
- Visual C++ runtime (usually already in `C:\Windows\System32`)
- JSL runtime DLLs present in:
  - `vendor\runtime\jsl\*.dll`
- Thrift controller script resolvable at runtime:
  - default auto-discovery from current repo (`src\thrift_controller_fullcycle.py`)
  - optional override: `RIP_THRIFT_CONTROLLER_PY=C:\full\path\to\thrift_controller_fullcycle.py`

## Why this exists

If `memjet-rip.exe` exits with `-1073741515` (`0xC0000135`), Windows is missing one or more runtime DLLs.

Keeping JSL DLLs in `vendor\runtime\jsl` and copying them next to the EXE during rebuild avoids PATH drift and "works on one machine only" failures.

## Quick health check

```bat
scripts\setup-env.bat
scripts\rebuild.bat
scripts\test-print.bat "C:\Users\Arrow\Downloads\toner-print-quality-sheets-colour.pdf"
```
