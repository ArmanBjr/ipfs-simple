# FUM OS Project 1 – Content-Addressed Storage

Python HTTP gateway + C core engine (IPC, thread pool). Chunking + hashing + manifests.

Quick Start (Windows)
- Gateway: cd gateway_py → python -m venv .venv → .\.venv\Scripts\activate → pip install -r requirements.txt → python main.py → open http://127.0.0.1:8000/health
- Engine: cd c_engine → make → .\build\engine (اگر make نداری فعلاً این بخش را رد کن)

Repo Layout
- gateway_py/ : Python HTTP gateway (FastAPI)
- c_engine/   : C core (IPC, thread pool)
- storage/    : chunks/ , manifests/ (ignored)
- tests/      : unit & e2e
- docs/       : design & architecture
