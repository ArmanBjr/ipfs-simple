# FUM OS Project - IPFS

Content-addressed storage system with FastAPI web gateway and C storage engine.

## Overview

This project implements an IPFS-like storage system where files are:

- **Chunked** into fixed-size blocks
- **Hashed** using Blake3 for content addressing
- **Stored** with deduplication
- **Accessed** via content identifiers (CIDs)

The system consists of two components:

- **Gateway**: FastAPI web interface for simple user authentication and file operations
- **Engine**: C-based storage engine handling chunking, hashing, and block storage

Communication between gateway and engine happens via UNIX domain sockets using a binary protocol.

## Quick Start

### 1. Build and Run Engine

```bash
cd engine
make
./engine /tmp/engine.sock
```

The engine listens on the specified socket path (default: `/tmp/engine.sock`).

### 2. Run Gateway

```bash
cd gateway
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

## Project Structure

```
.
├── engine/           # C storage engine
│   ├── src/          # Source files
│   ├── include/      # Header files
│   └── deps/         # Dependencies (Blake3)
├── gateway/          # FastAPI web gateway
│   ├── core/         # Core modules (config, engine, users)
│   ├── routers/      # API routes (auth, files, pages)
│   ├── templates/    # HTML templates
│   └── static/       # static files (CSS)
└── Makefile          # Build commands
```

## Features

- Content-addressed storage with Blake3 hashing
- Chunk-based deduplication
- User authentication and file ownership
- Web interface for upload/download
- Multi-threaded engine for concurrent operations
- UNIX socket IPC between gateway and engine
