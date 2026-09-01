# IPFS Simple

A simplified **content-addressed storage** system with a web gateway — inspired by IPFS. Built as the Operating Systems course project at Ferdowsi University of Mashhad (FUM, 1404).

Two services work together:

- **Engine** — high-performance C storage backend with a UNIX-socket binary API
- **Gateway** — FastAPI web UI for signup, upload, download, and file listing

## Features

- **Content-addressed storage** — files identified by Blake3 hash (CID) with automatic deduplication
- **Chunking** — files split into 256 KB blocks
- **User authentication** — signup/signin with session cookies
- **Owner tokens** — SHA-256 tokens for gateway ↔ engine authorization
- **Concurrent engine** — thread-pool request handling in C
- **Docker deployment** — `docker-compose` for one-command startup

## Tech stack

| Layer | Stack |
|-------|-------|
| Engine | C11, Blake3, pthreads, UNIX domain sockets |
| Gateway | Python 3, FastAPI, Jinja2, Uvicorn |
| Ops | Docker, Docker Compose, Make |

## Project structure

```
.
├── engine/                 # C storage engine
│   ├── src/                # Implementation
│   ├── include/            # Public headers
│   ├── deps/blake3/        # Vendored Blake3
│   └── Makefile
├── gateway/                # FastAPI web gateway
│   ├── core/               # Config, engine client, users
│   ├── routers/            # Auth, files, pages
│   ├── static/             # CSS
│   ├── templates/          # HTML (Jinja2)
│   └── requirements.txt
├── docker-compose.yml
├── Makefile
└── document.pdf            # Project report (Persian)
```

## Quick start (Docker)

```bash
git clone https://github.com/ArmanBjr/ipfs-simple.git
cd ipfs-simple
make up
```

Open **http://localhost:8000** — sign up, then upload and download files.

| Command | Description |
|---------|-------------|
| `make up` | Build and start engine + gateway |
| `make down` | Stop services (keep data) |
| `make reset` | Stop and wipe all volumes |
| `make logs` | Follow all logs |
| `make logs-engine` | Engine logs only |
| `make logs-gateway` | Gateway logs only |
| `make clean` | Remove build artifacts and runtime data |

## Local development

**Terminal 1 — engine:**

```bash
cd engine
make
./engine /tmp/engine.sock
```

**Terminal 2 — gateway:**

```bash
cd gateway
python -m venv .venv
# Windows:  .venv\Scripts\activate
# Linux:    source .venv/bin/activate
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

## Architecture

```
Browser  →  Gateway (FastAPI :8000)
                ↓  UNIX socket (/tmp/engine.sock)
            Engine (C)  →  blocks/  manifests/  owners/
```

**Data separation**

- Gateway stores `users.json` (authentication)
- Engine stores `blocks/`, `manifests/`, `owners/` (content)
- Shared socket at `/tmp/engine.sock`

**Binary protocol** (engine API)

- Upload: `OP_UPLOAD_START` → `OP_UPLOAD_CHUNK` → `OP_UPLOAD_FINISH`
- Download: `OP_DOWNLOAD_START` → `OP_DOWNLOAD_CHUNK` → `OP_DOWNLOAD_DONE`
- List: `OP_LIST_FILES` → `OP_LIST_RESPONSE`

## Documentation

See [`document.pdf`](document.pdf) for the full project report (design, implementation, and evaluation).

## Authors

- **Arman Bijari** — [GitHub](https://github.com/ArmanBjr)
- **Amir Mohammad Khorasani** — [GitHub](https://github.com/amirilf)

## License

MIT — see [LICENSE](LICENSE).
