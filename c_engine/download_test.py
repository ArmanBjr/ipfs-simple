#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import socket
import struct
import sys

SOCK_PATH = "/tmp/cengine.sock"

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE  = 0x92

def read_n(sock, n):
    """Read exactly n bytes from socket or return None on EOF."""
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def send_frame(sock, op, payload: bytes):
    """Send a frame: 1 byte op + 4 byte big-endian length + payload."""
    if payload is None:
        payload = b""
    header = struct.pack("!BI", op, len(payload))
    sock.sendall(header + payload)

def recv_frame(sock):
    """Receive a frame, return (op, payload_bytes) or (None, None) on EOF."""
    header = read_n(sock, 5)
    if header is None:
        return None, None
    op, length = struct.unpack("!BI", header)
    if length > 0:
        payload = read_n(sock, length)
        if payload is None:
            return None, None
    else:
        payload = b""
    return op, payload

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <CID> <output_filename>")
        print('Example:')
        print(f"  {sys.argv[0]} 0108BD0446414B454853 Test_downloaded.pdf")
        sys.exit(1)

    cid = sys.argv[1]
    out_name = sys.argv[2]

    # خروجی توی پوشه download_files
    out_dir = "download_files"
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, out_name)

    print(f"[CLIENT] Connecting to {SOCK_PATH}")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)

    # CID رو به‌صورت payload می‌فرستیم (بدون null terminator)
    payload = cid.encode("ascii")
    print(f"[CLIENT] Sending DOWNLOAD_START for CID={cid}")
    send_frame(s, OP_DOWNLOAD_START, payload)

    total_received = 0
    chunk_index = 0

    with open(out_path, "wb") as f:
        while True:
            op, data = recv_frame(s)
            if op is None:
                print("[CLIENT] EOF from server")
                break

            if op == OP_DOWNLOAD_CHUNK:
                chunk_index += 1
                f.write(data)
                total_received += len(data)
                print(f"[CLIENT] Got CHUNK #{chunk_index}, len={len(data)}, total={total_received}")
            elif op == OP_DOWNLOAD_DONE:
                print("[CLIENT] DOWNLOAD_DONE received")
                break
            else:
                print(f"[CLIENT] Unexpected op=0x{op:02X}, len={len(data)}")
                break

    s.close()
    print(f"[CLIENT] Download finished. Saved to: {out_path}")
    print(f"[CLIENT] Total bytes received = {total_received}")

if __name__ == "__main__":
    main()
