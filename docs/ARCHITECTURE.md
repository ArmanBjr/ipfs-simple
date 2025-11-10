# Architecture

## Overview
- Python HTTP Gateway (FastAPI)
- C Engine (IPC via Unix domain socket / named pipe)
- Storage layout: chunks/, manifests/

## Upload Flow
1. Receive file stream
2. Chunking → hash → send tasks to C engine
3. Build manifest → compute CID
4. Atomic write (fsync + rename)

## Download Flow
1. Resolve CID → manifest
2. Fetch chunks → stream to client

## IPC Protocol (Draft)
- Message types, framing, error codes

## Concurrency & Threading
- Thread pool (workers, queue)
- Locks & condition variables
