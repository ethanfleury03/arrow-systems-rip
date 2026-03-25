# RIP Adapter Service

Thin FastAPI adapter exposing:
- `GET /health`
- `POST /jobs`
- `GET /jobs/{id}`

## Run locally

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r adapter/requirements.txt
uvicorn adapter.service:app --host 0.0.0.0 --port 8080
```

Set RIP command (optional):

```bash
export RIP_COMMAND="./src/build/memjet-rip"
```

If not set, adapter uses `./src/build/memjet-rip`.
