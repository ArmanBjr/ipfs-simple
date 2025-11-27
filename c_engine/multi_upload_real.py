#!/usr/bin/env python3
import os
import socket
import struct
import threading
import hashlib
from pathlib import Path

# === Protocol opcodes (باید با c_engine یکی باشه) ===
OP_UPLOAD_START   = 0x01
OP_UPLOAD_CHUNK   = 0x02
OP_UPLOAD_FINISH  = 0x03
OP_UPLOAD_DONE    = 0x81

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE  = 0x92

SOCK_PATH = "/tmp/cengine.sock"
CHUNK_SIZE = 256 * 1024  # فقط سایز خواندن در کلاینت؛ مستقل از سرور

# ---------- helper های پروتکل ----------

def send_frame(sock: socket.socket, op: int, payload: bytes):
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
    header = recv_exact(sock, 5)  # 1 + 4
    op, length = struct.unpack("!BI", header)
    payload = recv_exact(sock, length) if length > 0 else b""
    return op, payload

# ---------- hash helper ----------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

# ---------- upload / download ----------

def upload_file(path: Path) -> str:
    size = path.stat().st_size
    name_bytes = path.name.encode("utf-8")

    print(f"[UPLOAD] {path.name}: size={size} bytes")

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)

    # UPLOAD_START payload: 8 بایت total_size (big-endian) + filename
    payload = struct.pack("!Q", size) + name_bytes
    print(f"[UPLOAD] {path.name}: sending UPLOAD_START ...")
    send_frame(s, OP_UPLOAD_START, payload)

    # stream data as CHUNK frames
    print(f"[UPLOAD] {path.name}: streaming data ...")
    with path.open("rb") as f:
        while True:
            data = f.read(CHUNK_SIZE)
            if not data:
                break
            send_frame(s, OP_UPLOAD_CHUNK, data)

    # finish
    print(f"[UPLOAD] {path.name}: sending UPLOAD_FINISH ...")
    send_frame(s, OP_UPLOAD_FINISH, b"")

    # wait for UPLOAD_DONE
    cid = None
    while True:
        op, payload = recv_frame(s)
        if op == OP_UPLOAD_DONE:
            cid = payload.decode("ascii")
            break
        else:
            print(f"[UPLOAD] {path.name}: unexpected op {op:#x} while waiting for UPLOAD_DONE")

    s.close()
    print(f"[UPLOAD] {path.name}: DONE, CID={cid}")
    return cid

def download_file(cid: str, out_path: Path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)

    print(f"[DOWNLOAD] {cid}: saving to {out_path}")
    send_frame(s, OP_DOWNLOAD_START, cid.encode("ascii"))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        while True:
            op, payload = recv_frame(s)
            if op == OP_DOWNLOAD_CHUNK:
                f.write(payload)
            elif op == OP_DOWNLOAD_DONE:
                break
            else:
                print(f"[DOWNLOAD] {cid}: unexpected op {op:#x}")
                break

    s.close()
    print(f"[DOWNLOAD] {cid}: finished, written to {out_path}")

# ---------- worker برای هر فایل ----------

def worker_upload_download(src_path: Path, download_dir: Path):
    try:
        src_hash = sha256_file(src_path)

        cid = upload_file(src_path)

        dst_path = download_dir / src_path.name
        download_file(cid, dst_path)

        dst_hash = sha256_file(dst_path)

        if src_hash == dst_hash:
            print(f"[VERIFY] {src_path.name}: OK")
        else:
            print(f"[VERIFY] {src_path.name}: MISMATCH!")
            print(f"         src={src_hash}")
            print(f"         dst={dst_hash}")
    except Exception as e:
        print(f"[ERROR] {src_path.name}: {e}")

# ---------- main ----------

def main():
    base_dir = Path(".").resolve()
    upload_dir = base_dir / "upload_test_files"
    download_dir = base_dir / "download_files"

    # همون لیستی که گفتی:
    files = [
        upload_dir / "Test0",       # بدون پسوند
        upload_dir / "Test1.pdf",
        upload_dir / "Test2.pdf",
        upload_dir / "Test3.mp3",
        upload_dir / "Test4.mp4",
    ]

    print(f"[TEST] Upload dir = {upload_dir}")
    print(f"[TEST] Download dir = {download_dir}")

    for p in files:
        if not p.exists():
            print(f"[WARN] File does not exist, skipping: {p}")
    files = [p for p in files if p.exists()]
    if not files:
        print("[TEST] No files found, exiting.")
        return

    print("\n[TEST] Starting multi-threaded upload + download for:")
    for p in files:
        print(f"       - {p.name} ({p.stat().st_size} bytes)")

    threads = []
    for p in files:
        t = threading.Thread(target=worker_upload_download, args=(p, download_dir), daemon=True)
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    print("\n[TEST] All threads finished.")

if __name__ == "__main__":
    main()
