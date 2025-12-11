# IPFS Gateway

A content-addressed storage system with a web gateway interface. The project consists of two main components:

- **Engine**: High-performance C storage engine with UNIX socket API
- **Gateway**: FastAPI web interface for user interaction

## Features

- **Content-Addressed Storage**: Files stored by hash (CID) with automatic deduplication
- **Chunking**: Automatic file splitting into 256KB chunks
- **Blake3 Hashing**: Fast cryptographic hashing for content identification
- **User Authentication**: Secure signup/signin with session management
- **Owner Tokens**: SHA-256 based authentication tokens for engine communication
- **File Operations**: Upload, download, and list files via web interface
- **Thread Pool**: Concurrent request handling in the C engine
- **Docker Support**: Fully containerized deployment

## Project Structure

```
.
├── engine/              # C storage engine
│   ├── src/             # Source files
│   ├── include/         # Header files
│   ├── deps/            # Dependencies (Blake3)
│   └── Makefile         # Build configuration
├── gateway/             # FastAPI web gateway
│   ├── core/            # Core modules (config, engine client, users)
│   ├── routers/         # Route handlers (auth, files, pages)
│   ├── static/          # CSS and assets
│   ├── templates/       # Jinja2 HTML templates
│   └── requirements.txt
├── docker-compose.yml
└── Makefile
```

## Quick Start

### Using Docker

```bash
# start both services
make up

# view logs (all services)
make logs

# view logs separately (use different terminals)
make logs-engine
make logs-gateway

# stop services
make down

# complete reset (removes all data and volumes)
make reset
```

### Local

```bash
# run engine in first terminal
cd engine
make
./engine /tmp/engine.sock

# run gatewat in second terminal
cd gateway
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

## Makefile Commands

| Command             | Description                               |
| ------------------- | ----------------------------------------- |
| `make up`           | Build and start all services              |
| `make down`         | Stop all services (keeps data)            |
| `make reset`        | Stop services and remove all volumes/data |
| `make logs`         | View logs from all services               |
| `make logs-engine`  | View engine logs only                     |
| `make logs-gateway` | View gateway logs only                    |
| `make clean`        | Clean build artifacts and cache           |

### Data Separation

- **Gateway**: Manages `users.json` (authentication)
- **Engine**: Manages `blocks/`, `manifests/`, `owners/` (storage)
- **Socket**: Shared at `/tmp/engine.sock`

### Binary Protocol

The engine communicates via a custom binary protocol over UNIX sockets:

- `OP_UPLOAD_START`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_FINISH`
- `OP_DOWNLOAD_START`, `OP_DOWNLOAD_CHUNK`, `OP_DOWNLOAD_DONE`
- `OP_LIST_FILES`, `OP_LIST_RESPONSE`
