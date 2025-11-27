#!/usr/bin/env python3
import socket
import struct
import threading
import hashlib
from pathlib import Path

# Protocol opcodes (must match c_engine)
OP_UPLOAD_START   = 0x01
OP_UPLOAD_CHUNK   = 0x02
OP_UPLOAD_FINISH  = 0x03
OP_UPLOAD_DONE    = 0x81

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE  = 0x92

SOCK_PATH = "/tmp/cengine.sock"


# ---------- framing helpers ----------

def send_frame(sock: socket.socket, op: int, payload: bytes | None):
    if payload is None:
        payload = b""
    header = struct.pack("!BI", op, len(payload))
    sock.sendall(header + payload)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise RuntimeError("socket closed while recv_exact")
        data += chunk
    return data


def recv_frame(sock: socket.socket):
    header = recv_exact(sock, 5)  # 1 byte op + 4 byte length
    op, length = struct.unpack("!BI", header)
    payload = recv_exact(sock, length) if length > 0 else b""
    return op, payload


# ---------- hash helpers ----------

def sha256_bytes(data: bytes) -> str:
    h = hashlib.sha256()
    h.update(data)
    return h.hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


# ---------- core upload / download (no client-side chunking) ----------

def upload_file_single_chunk(path: Path) -> str:
    size = path.stat().st_size
    name_bytes = path.name.encode("utf-8")

    print(f"[UPLOAD] {path.name}: size={size} bytes")

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)

    # UPLOAD_START: 8 bytes total_size (big-endian) + filename bytes
    payload = struct.pack("!Q", size) + name_bytes
    print(f"[UPLOAD] {path.name}: sending UPLOAD_START")
    send_frame(s, OP_UPLOAD_START, payload)

    # Read whole file once and send as a single CHUNK
    with path.open("rb") as f:
        data = f.read()
    print(f"[UPLOAD] {path.name}: sending single UPLOAD_CHUNK (len={len(data)})")
    send_frame(s, OP_UPLOAD_CHUNK, data)

    print(f"[UPLOAD] {path.name}: sending UPLOAD_FINISH")
    send_frame(s, OP_UPLOAD_FINISH, b"")

    # Wait for UPLOAD_DONE and get CID
    cid = None
    while True:
        op, payload = recv_frame(s)
        if op == OP_UPLOAD_DONE:
            cid = payload.decode("ascii")
            break
        else:
            print(f"[UPLOAD] {path.name}: unexpected op while waiting for UPLOAD_DONE: 0x{op:02x}")

    s.close()
    print(f"[UPLOAD] {path.name}: DONE, CID={cid}")
    return cid


def download_to_bytes(cid: str) -> bytes:
    print(f"[DOWNLOAD] {cid}: starting download")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)

    send_frame(s, OP_DOWNLOAD_START, cid.encode("ascii"))

    chunks: list[bytes] = []

    while True:
        op, payload = recv_frame(s)
        if op == OP_DOWNLOAD_CHUNK:
            chunks.append(payload)
        elif op == OP_DOWNLOAD_DONE:
            break
        else:
            print(f"[DOWNLOAD] {cid}: unexpected op 0x{op:02x}")
            break

    s.close()
    data = b"".join(chunks)
    print(f"[DOWNLOAD] {cid}: finished, total_len={len(data)} bytes")
    return data


# ---------- scenarios ----------

def scenario_sequential(files: list[Path]):
    print("\n========== SCENARIO 1: SEQUENTIAL UPLOAD + DOWNLOAD ==========")
    for p in files:
        try:
            print(f"\n[SC1] File: {p.name}")
            orig_hash = sha256_file(p)
            cid = upload_file_single_chunk(p)
            data = download_to_bytes(cid)
            dl_hash = sha256_bytes(data)

            if orig_hash == dl_hash:
                print(f"[SC1][VERIFY] {p.name}: OK (hash={orig_hash})")
            else:
                print(f"[SC1][VERIFY] {p.name}: MISMATCH!")
                print(f"    orig={orig_hash}")
                print(f"    down={dl_hash}")
        except Exception as e:
            print(f"[SC1][ERROR] {p.name}: {e}")


def worker_parallel(path: Path):
    try:
        print(f"\n[SC2][THREAD] starting for {path.name}")
        orig_hash = sha256_file(path)
        cid = upload_file_single_chunk(path)
        data = download_to_bytes(cid)
        dl_hash = sha256_bytes(data)

        if orig_hash == dl_hash:
            print(f"[SC2][VERIFY] {path.name}: OK (hash={orig_hash})")
        else:
            print(f"[SC2][VERIFY] {path.name}: MISMATCH!")
            print(f"    orig={orig_hash}")
            print(f"    down={dl_hash}")
    except Exception as e:
        print(f"[SC2][ERROR] {path.name}: {e}")


def scenario_parallel(files: list[Path]):
    print("\n========== SCENARIO 2: PARALLEL UPLOAD + DOWNLOAD ==========")
    threads = []
    for p in files:
        t = threading.Thread(target=worker_parallel, args=(p,), daemon=True)
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    print("[SC2] All threads finished.")


# ---------- main ----------

def main():
    base_dir = Path(".").resolve()
    upload_dir = base_dir / "upload_test_files"

    print(f"[TEST] Using upload directory: {upload_dir}")

    if not upload_dir.exists():
        print("[TEST] upload_test_files directory does not exist.")
        return

    files = sorted(p for p in upload_dir.iterdir() if p.is_file())
    if not files:
        print("[TEST] No files found in upload_test_files.")
        return

    print("[TEST] Files to test:")
    for p in files:
        print(f"   - {p.name} ({p.stat().st_size} bytes)")

    # Scenario 1: sequential
    scenario_sequential(files)

    # Scenario 2: parallel
    scenario_parallel(files)


if __name__ == "__main__":
    main()
