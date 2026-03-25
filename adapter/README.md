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

## Default PES routing (recommended)

Set stable defaults once and keep submit payloads simple:

```bash
export RIP_DEFAULT_PES_IP="192.168.111.2"
export RIP_DEFAULT_PES_PORT="13001"
```

`POST /jobs` can then use only:

```json
{"input_path":"C:\\Users\\Arrow\\Downloads\\file.pdf"}
```

Override behavior remains supported:
- payload `args` with `--pes-ip/--pes-port` wins over defaults
- `--dry-run` does not inject PES defaults
