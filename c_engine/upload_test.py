#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Simple Python client for the C engine.

Protocol:
- Frames are:
    [1 byte opcode][4 bytes length (big-endian)][payload bytes]

- Upload sequence:
    1) OP_UPLOAD_START (0x01)
       payload = [8 bytes total_size (big-endian)] + [filename bytes, no '\0']

    2) Repeated OP_UPLOAD_CHUNK (0x02)
       payload = raw file bytes (any size per frame)

    3) OP_UPLOAD_FINISH (0x03)
       payload = empty

    4) Server replies with OP_UPLOAD_DONE (0x81)
       payload = CID string (UTF-8)
"""

import os
import sys
import socket
import struct


# Adjust this if your engine uses a different path
SOCKET_PATH = "/tmp/cengine.sock"

# Opcodes (must match protocol.h / server)
OP_UPLOAD_START  = 0x01
OP_UPLOAD_CHUNK  = 0x02
OP_UPLOAD_FINISH = 0x03
OP_UPLOAD_DONE   = 0x81


def send_frame(sock: socket.socket, opcode: int, payload: bytes) -> None:
    """Send one frame: [opcode][4-byte len big-endian][payload]."""
    if payload is None:
        payload = b""
    header = struct.pack("!BI", opcode, len(payload))
    sock.sendall(header + payload)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Receive exactly n bytes or raise RuntimeError on EOF."""
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("Connection closed while reading")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_frame(sock: socket.socket):
    """Receive one frame and return (opcode, payload_bytes)."""
    header = recv_exact(sock, 5)  # 1 byte opcode + 4 bytes length
    opcode, length = struct.unpack("!BI", header)
    if length > 0:
        payload = recv_exact(sock, length)
    else:
        payload = b""
    return opcode, payload


def upload_file(file_path: str) -> None:
    """Upload a single file to the C engine and print the returned CID."""
    if not os.path.isfile(file_path):
        print(f"[CLIENT] ERROR: file not found: {file_path}")
        return

    # Get file size and base name
    total_size = os.path.getsize(file_path)
    filename = os.path.basename(file_path)
    print(f"[CLIENT] Uploading '{filename}' ({total_size} bytes)")

    # Connect to Unix domain socket
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        print(f"[CLIENT] Connecting to {SOCKET_PATH} ...")
        sock.connect(SOCKET_PATH)

        # 1) Send OP_UPLOAD_START
        total_size_be = struct.pack("!Q", total_size)  # uint64 big-endian
        fname_bytes = filename.encode("utf-8")
        payload_start = total_size_be + fname_bytes

        print("[CLIENT] Sending UPLOAD_START ...")
        send_frame(sock, OP_UPLOAD_START, payload_start)

        # 2) Send file data as stream via OP_UPLOAD_CHUNK
        print("[CLIENT] Sending file data as stream (UPLOAD_CHUNK frames) ...")
        with open(file_path, "rb") as f:
            while True:
                chunk = f.read(64 * 1024)  # 64 KB per frame (arbitrary)
                if not chunk:
                    break
                send_frame(sock, OP_UPLOAD_CHUNK, chunk)

        # 3) Send OP_UPLOAD_FINISH
        print("[CLIENT] Sending UPLOAD_FINISH ...")
        send_frame(sock, OP_UPLOAD_FINISH, b"")

        # 4) Wait for OP_UPLOAD_DONE and extract CID
        print("[CLIENT] Waiting for UPLOAD_DONE ...")
        while True:
            opcode, payload = recv_frame(sock)
            if opcode == OP_UPLOAD_DONE:
                cid = payload.decode("utf-8", errors="replace")
                print(f"[CLIENT] Upload complete. CID = {cid}")
                break
            else:
                # For now, just log unexpected frames and continue
                print(f"[CLIENT] Received unexpected frame opcode=0x{opcode:02x}, len={len(payload)}")

    except FileNotFoundError:
        print(f"[CLIENT] ERROR: socket path not found: {SOCKET_PATH}")
    except ConnectionRefusedError:
        print("[CLIENT] ERROR: connection refused (is the server running?)")
    except Exception as e:
        print(f"[CLIENT] ERROR: {e}")
    finally:
        sock.close()


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 upload_test.py <file_path>")
        print("Example: python3 upload_test.py Test1.pdf")
        sys.exit(1)

    file_path = sys.argv[1]
    upload_file(file_path)


if __name__ == "__main__":
    main()
