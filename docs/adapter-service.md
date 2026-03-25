# RIP Adapter Service Contract

## Endpoints

### GET /health
Response 200:
```json
{"ok": true, "service": "rip-adapter", "time": "2026-03-25T13:00:00Z"}
```

### POST /jobs
Request:
```json
{
  "input_path": "C:/print/jobs/demo.pdl",
  "args": ["--mode", "prod"],
  "env": {"USE_TRUE_CMYK": "1"}
}
```

Response 202:
```json
{"id":"<uuid>","status":"queued","command":["./src/build/memjet-rip","C:/print/jobs/demo.pdl","--mode","prod"]}
```

### GET /jobs/{id}
Response 200:
```json
{
  "id": "<uuid>",
  "status": "running",
  "created_at": "...",
  "updated_at": "...",
  "payload": {"input_path": "...", "args": [], "env": {}},
  "events": [{"event": "rip.job.created"}],
  "logs_tail": ["..."],
  "exit_code": null,
  "error_code": null
}
```

## State mapping
- `queued` at submission
- `running` after process start
- JSON event `rip.job.created` => `preparing`
- JSON event `rip.completed` => `completed`
- JSON event `rip.failed` => `failed`
- nonzero exit without event => `failed` + `RIP_RUNTIME_EXCEPTION`

## Quickstart
1. `python3 -m venv .venv && source .venv/bin/activate`
2. `pip install -r adapter/requirements.txt`
3. `uvicorn adapter.service:app --host 0.0.0.0 --port 8080`

## cURL examples
```bash
curl -s http://localhost:8080/health
```

```bash
curl -s -X POST http://localhost:8080/jobs -H 'Content-Type: application/json' -d '{"input_path":"/tmp/sample.pdl"}'
```

```bash
curl -s http://localhost:8080/jobs/<job-id>
```
