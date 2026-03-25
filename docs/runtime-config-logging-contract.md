# Runtime Config + Logging/Error Contract (starter)

## Runtime config keys (env)

Centralized in `src/config.{h,cpp}` and loaded at startup.

- Render mode
  - `USE_FAST_MONO` (bool)
  - `USE_TRUE_CMYK` (bool)
- Baseline/tuning
  - `RIP_BASELINE_PROFILE` (`NONE` | `ANYFLOW_V1`)
  - `RIP_INK_LIMIT` (0..1 or 0..100)
  - `RIP_C_SCALE`, `RIP_M_SCALE`, `RIP_Y_SCALE`, `RIP_K_SCALE` (0..2)
  - `RIP_THRESHOLD_BIAS` (-64..64)
- JSL payload
  - `JSL_INVERT_BITS` (bool)
  - `JSL_TEST_PATTERN` (bool)
  - `JSL_STRIP_START` (int >= 0)
  - `JSL_STRIP_WIDTH` (int >= 0)
- Thrift/PES
  - `THRIFT_CONTROLLER_PATH`
  - `THRIFT_CONTROL_PORT` (1..65535)
  - `THRIFT_PYTHON_EXE`
  - `PDL_THRIFT_ROOT`
  - `USE_LEGACY_ORCHESTRATION` (bool)
  - `THRIFT_WAIT_JOB_TIMEOUT_MS` (1..300000)
  - `THRIFT_WAIT_JOB_POLL_MS` (1..30000)
  - `JSL_POST_START_HOLD_MS` (0..300000)
  - `JSL_IMMEDIATE_FINISH` (bool)

Invalid values return `RIP_CONFIG_INVALID` and non-zero exit code.

## Structured log schema

`src/logger.{h,cpp}` emits JSON lines while preserving existing human-readable `[INFO]/[ERROR]` logs.

Base shape:

```json
{
  "ts": "2026-03-25T13:00:00.123Z",
  "component": "memjet-rip",
  "level": "INFO|WARN|ERROR",
  "event": "optional.event.name",
  "message": "optional text",
  "error_code": "optional standardized code",
  "...": "additional string fields"
}
```

Current event names:
- `rip.job.created`
- `rip.completed`
- `rip.failed`

## Standardized error code set

Defined in `src/error_codes.{h,cpp}`:

- `OK` (0)
- `RIP_INVALID_ARGS` (2)
- `RIP_INPUT_FILE_MISSING` (3)
- `RIP_CONFIG_INVALID` (4)
- `RIP_RASTERIZER_INIT_FAILED` (10)
- `RIP_RASTERIZATION_FAILED` (11)
- `RIP_PLANE_CONVERSION_FAILED` (12)
- `RIP_JSL_INIT_FAILED` (20)
- `RIP_JSL_SUBMISSION_FAILED` (21)
- `RIP_VERIFICATION_FAILED` (30)
- `RIP_RUNTIME_EXCEPTION` (99)

UI consumers should key off `error_code` and `event` first, with `message` as display text.
