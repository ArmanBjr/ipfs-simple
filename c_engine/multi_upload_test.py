#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import socket
import threading
import hashlib
import random
import string
from pathlib import Path
from typing import Tuple

# Unix domain socket path of the C engine
SOCK_PATH = "/tmp/cengine.sock"

# Opcodes (must match c_engine.c)
OP_UPLOAD_START   = 0x01
OP_UPLOAD_CHUNK   = 0x02
OP_UPLOAD_FINISH  = 0x03
OP_UPLOAD_DONE    = 0x81

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE  = 0x92

# Chunk size for client-side streaming (server will re-chunk internally)
CLIENT_CHUNK_SIZE = 64 * 1024  # 64 KiB


# ------------------ Low-level frame helpers ------------------ #

def send_frame(sock: socket.socket, op: int, payload: bytes) -> None:
    """Send a frame: 1 byte opcode + 4 bytes big-endian length + payload."""
    length = len(payload)
    header = bytes([op]) + length.to_bytes(4, "big")
    sock.sendall(header + payload)


def recv_frame(sock: socket.socket) -> Tuple[int, bytes]:
    """Receive a frame from the server. Returns (op, payload)."""
    header = b""
    while len(header) < 5:
        chunk = sock.recv(5 - len(header))
        if not chunk:
            raise ConnectionError("EOF while reading frame header")
        header += chunk

    op = header[0]
    length = int.from_bytes(header[1:5], "big")

    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise ConnectionError("EOF while reading frame payload")
        payload += chunk

    return op, payload


# ------------------ Upload / Download logic ------------------ #

def upload_file(path: Path) -> str:
    """Upload a single file, return CID as string."""
    size = path.stat().st_size
    filename = path.name.encode("utf-8")

    print(f"[UPLOAD] {path.name}: size={size} bytes")

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(SOCK_PATH)

        # UPLOAD_START: [0..7] total_size (big-endian) + filename bytes
        payload_start = size.to_bytes(8, "big") + filename
        print(f"[UPLOAD] {path.name}: sending UPLOAD_START ...")
        send_frame(sock, OP_UPLOAD_START, payload_start)

        # Stream file as UPLOAD_CHUNK frames
        print(f"[UPLOAD] {path.name}: streaming data ...")
        with path.open("rb") as f:
            while True:
                chunk = f.read(CLIENT_CHUNK_SIZE)
                if not chunk:
                    break
                send_frame(sock, OP_UPLOAD_CHUNK, chunk)

        # Finish upload
        print(f"[UPLOAD] {path.name}: sending UPLOAD_FINISH ...")
        send_frame(sock, OP_UPLOAD_FINISH, b"")

        # Expect UPLOAD_DONE with CID as payload
        op, payload = recv_frame(sock)
        if op != OP_UPLOAD_DONE:
            raise RuntimeError(f"[UPLOAD] {path.name}: expected OP_UPLOAD_DONE, got 0x{op:02x}")

        cid = payload.decode("utf-8")
        print(f"[UPLOAD] {path.name}: DONE, CID={cid}")
        return cid


def download_file(cid: str, out_path: Path) -> None:
    """Download a file by CID and write it to out_path."""
    print(f"[DOWNLOAD] {cid}: saving to {out_path}")

    out_path.parent.mkdir(parents=True, exist_ok=True)

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(SOCK_PATH)

        # Send DOWNLOAD_START with CID bytes
        send_frame(sock, OP_DOWNLOAD_START, cid.encode("utf-8"))

        # Receive chunks until DOWNLOAD_DONE
        with out_path.open("wb") as f:
            while True:
                op, payload = recv_frame(sock)
                if op == OP_DOWNLOAD_CHUNK:
                    f.write(payload)
                elif op == OP_DOWNLOAD_DONE:
                    break
                else:
                    raise RuntimeError(
                        f"[DOWNLOAD] {cid}: unexpected opcode 0x{op:02x}"
                    )

    print(f"[DOWNLOAD] {cid}: finished, written to {out_path}")


# ------------------ Test file generation ------------------ #

def random_text(size: int) -> str:
    """Generate random ASCII text of given size (roughly)."""
    alphabet = string.ascii_letters + string.digits + " .,!?\n"
    return "".join(random.choice(alphabet) for _ in range(size))


def generate_test_files(base_dir: Path) -> None:
    """Generate a set of test files with different sizes and types."""
    base_dir.mkdir(parents=True, exist_ok=True)

    # Test0.txt: ~50 KB text
    (base_dir / "Test0.txt").write_text(random_text(50 * 1024), encoding="utf-8")

    # These are just random bytes; extensions are only for realism
    def write_random_binary(name: str, size: int):
        (base_dir / name).write_bytes(os.urandom(size))

    # Rough sizes: you can adjust as you like
    write_random_binary("Test1.pdf", 2 * 1024 * 1024)   # ~2 MB
    write_random_binary("Test2.pdf", 7 * 1024 * 1024)   # ~7 MB
    write_random_binary("Test3.mp3", 3 * 1024 * 1024)   # ~3 MB
    write_random_binary("Test4.mp4", 9 * 1024 * 1024)   # ~9 MB

    print(f"[TEST] Generated test files in {base_dir}")


# ------------------ Verification ------------------ #

def file_sha256(path: Path) -> str:
    """Compute SHA-256 of a file."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_equal(original: Path, downloaded: Path) -> bool:
    """Check if two files are exactly equal via SHA-256."""
    h1 = file_sha256(original)
    h2 = file_sha256(downloaded)
    equal = (h1 == h2)
    status = "OK" if equal else "MISMATCH"
    print(f"[VERIFY] {original.name}: {status}")
    if not equal:
        print(f"         original : {h1}")
        print(f"         downloaded: {h2}")
    return equal


# ------------------ Thread worker ------------------ #

def upload_and_download_worker(src: Path, download_dir: Path):
    """Thread routine: upload + download + verify one file."""
    try:
        cid = upload_file(src)
        out_path = download_dir / src.name
        download_file(cid, out_path)
        verify_equal(src, out_path)
    except Exception as e:
        print(f"[ERROR] Worker for {src.name}: {e}")


# ------------------ Main test runner ------------------ #

def main():
    # Assume we run inside c_engine directory
    base_dir = Path(".").resolve()
    test_files_dir = base_dir / "test_files"
    download_dir = base_dir / "download_files"

    # 1) Generate test files
    generate_test_files(test_files_dir)

    # 2) Prepare list of files to test
    files = [
        test_files_dir / "Test0.txt",
        test_files_dir / "Test1.pdf",
        test_files_dir / "Test2.pdf",
        test_files_dir / "Test3.mp3",
        test_files_dir / "Test4.mp4",
    ]

    print("\n[TEST] Starting multi-threaded upload + download for:")
    for f in files:
        print(f"       - {f.name} ({f.stat().st_size} bytes)")

    # 3) Run upload+download in parallel threads
    threads = []
    for f in files:
        t = threading.Thread(
            target=upload_and_download_worker,
            args=(f, download_dir),
            name=f"worker-{f.name}",
            daemon=False,
        )
        threads.append(t)
        t.start()

    # 4) Wait for all threads to finish
    for t in threads:
        t.join()

    print("\n[TEST] All threads finished.")
    print("[TEST] If server logs show interleaved [UPLOAD] worker processed chunk index=...,")
    print("       then multi-threaded upload is working as expected.\n")


if __name__ == "__main__":
    main()
