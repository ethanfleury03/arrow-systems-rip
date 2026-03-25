# RIP Production Readiness Plan (UI + Backend + CI/CD)

## 1) Current-state audit (this repo)

### Repo scope observed
- Core backend RIP executable in C++ (`src/memjet-rip.cpp` + supporting files).
- Thrift PES controller script in Python 2.7 (`src/thrift_controller_fullcycle.py`).
- Build via CMake (`src/CMakeLists.txt`) and Windows batch scripts (`scripts/setup-env.bat`, `scripts/rebuild.bat`, `scripts/test-print.bat`).
- Vendor/runtime expectations documented (`REQUIRED_RUNTIME.md`).
- No in-repo UI app code (no frontend package or web/electron UI in this repository).

### Integration points found
- RIP -> PES data path through JSL wrapper (`src/jsl_wrapper.*`, called from `src/memjet-rip.cpp`).
- RIP -> PES control path through Thrift full-cycle script (`src/thrift_controller_fullcycle.py`, called via `std::system` from `src/memjet-rip.cpp`).
- Verification path through Gymea logs (`verifyPrintExecution(...)` flow in RIP main path).

### Tests / quality gates
- No unit/integration test suite in this repo.
- No `ctest`, `pytest`, or automated contract test wiring present in this repo.
- No pre-merge CI workflow existed in this repo before this plan.

### Deployment/release scripts
- Manual/local Windows scripts only.
- No staged environment deployment automation (dev/staging/prod) in-repo.
- No semantic versioning or release pipeline in-repo.

## 2) Top production gaps

### Reliability / error handling
1. `std::system(...)` orchestration is brittle; lacks structured retry/backoff and typed failure handling.
2. Runtime path assumptions are hardcoded for specific Windows paths (Python, thrift script roots).
3. No circuit-breaker/recoverable-state strategy if PES control works but data submission partially fails.
4. No soak/regression test automation for critical print workflows.

### Config / environment management
1. Many env vars are ad hoc with no central schema/validation.
2. Hardcoded defaults for machine-specific paths (`C:\Python27\python.exe`, etc.).
3. No environment profiles (`.env.dev`, `.env.staging`, `.env.prod`) or typed config loading.

### Observability / logging
1. Logging appears mostly plain text; no structured JSON logs.
2. No correlation IDs propagated through all steps besides generated job ID text logs.
3. No metrics export (latency, retry counts, failure categories, queue dwell, print completion SLA).
4. No log shipping/aggregation contract for production troubleshooting.

### API contracts between UI and RIP
1. No explicit versioned API in this repo for UI consumption (CLI-only integration surface).
2. No OpenAPI/JSON schema/IDL contract for UI↔RIP interactions.
3. No contract tests validating payload compatibility between UI and RIP backend.

### Security basics
1. No secret handling standard for credentials/network config in CI runtime.
2. External command invocation via shell increases injection/escaping risk if inputs are not tightly constrained.
3. No dependency/SAST scanning baseline configured.
4. Python 2.7 dependency in production control path is an operational/security risk.

### Release/versioning
1. No formal semantic version/tag discipline and changelog automation.
2. No signed/reproducible release artifact process.
3. No rollback playbook integrated with release process.

## 3) Recommended architecture for UI↔RIP integration

## Target integration model
- Introduce a thin **RIP service adapter** in front of current CLI execution:
  - Input: typed JSON request from UI (job settings, file refs, print target profile).
  - Execution: invokes current RIP pipeline (existing C++ binary + orchestration) with validated parameters.
  - Output: structured job state model (`queued`, `preparing`, `sending`, `printing`, `completed`, `failed`) and reason codes.

### Contract-first recommendation
- Define `docs/contracts/rip-job.schema.json` for request/response envelopes.
- Version the contract (`contractVersion`, e.g., `v1`).
- UI should only depend on contract, not internal env vars or script paths.

### Near-term implementation shape
1. Keep existing C++ RIP unchanged for printing correctness.
2. Add a small adapter process (can be Node/Python/C++) to mediate UI calls.
3. Persist job events with stable IDs (use generated `jobId` + request UUID).
4. Emit machine-readable status stream/file for UI polling/subscription.

## 4) CI/CD design (recommended)

### CI (GitHub Actions)
Required checks:
1. **Build job (Windows)**: CMake configure + build Release, upload exe artifact.
2. **Static checks (next step)**:
   - C++ formatting/lint (`clang-format --dry-run`, `clang-tidy` where feasible).
   - Python lint for controller script (with Python 2 compatibility constraints documented).
3. **Test job (next step)**:
   - Add smoke tests for argument parsing and dry-run behavior.
   - Add integration-mock tests for orchestration state transitions.

### Branch protections
- Protect `main`:
  - Require PR.
  - Require CI baseline workflow green.
  - Require at least 1 review.
  - Require branch up-to-date before merge.

### CD / staged deploy strategy
- **Dev**: every merge to `main` publishes unsigned build artifact for internal validation.
- **Staging**: manually promoted build runs hardware-in-the-loop print validation checklist.
- **Prod**: tagged release (`vX.Y.Z`) promoted after staging sign-off + rollback artifact retained.

## 5) Phased execution plan

### Today (high-impact, low-risk)
1. Add baseline CI build workflow (done in this branch).
2. Add production-readiness plan + explicit gap register (this document).
3. Add issue templates/checklists for release gate criteria.

### This week (priority order)
1. **Config hardening**
   - Add centralized config module/struct with validation.
   - File-level targets:
     - `src/memjet-rip.cpp` (replace scattered env parsing with validated config object).
     - `docs/configuration.md` (new; canonical env var definitions).
2. **Structured observability**
   - Add JSON log mode + reason codes.
   - File-level targets:
     - `src/utils.h`, `src/utils.cpp`, `src/memjet-rip.cpp`.
3. **Contract definition for UI integration**
   - New files:
     - `docs/contracts/rip-job.schema.json`
     - `docs/contracts/rip-status.schema.json`
     - `docs/contracts/README.md`
4. **Reliability improvements in orchestration**
   - Wrap shell-out control path with strict escaping, timeout, retry policy, and failure taxonomy.
   - File-level targets:
     - `src/pes_orchestrator.h`, `src/pes_orchestrator.cpp`, `src/memjet-rip.cpp`.
5. **Initial automated tests**
   - Add tests for config parsing and orchestration state mapping.
   - New files:
     - `tests/` (new tree)
     - `CMakeLists` updates for test target(s).

## 6) CI baseline implemented now

Added:
- `.github/workflows/ci-baseline.yml`
  - Trigger: push + pull_request
  - Job: Windows configure/build via CMake
  - Artifact upload: `memjet-rip.exe`

Notes:
- Kept intentionally minimal to avoid risky infra assumptions (no printer/network dependencies, no runtime DLL assumptions in CI).
- Designed as first required check for branch protection.
