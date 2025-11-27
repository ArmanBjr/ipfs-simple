#!/usr/bin/env python3
import os
import sys
import socket
import struct

# Must match c_engine.c
SOCK_PATH = "/tmp/cengine.sock"

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE  = 0x92


def send_frame(sock: socket.socket, op: int, payload: bytes | None) -> None:
    """Send a single protocol frame: [1 byte op][4 bytes len][payload]."""
    if payload is None:
        payload = b""
    header = struct.pack("!BI", op, len(payload))
    sock.sendall(header + payload)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Receive exactly n bytes or raise on EOF."""
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise RuntimeError("Unexpected EOF while reading from socket")
        data += chunk
    return data


def recv_frame(sock: socket.socket) -> tuple[int, bytes]:
    """Receive a single protocol frame, return (op, payload)."""
    header = recv_exact(sock, 5)  # 1 byte op + 4 bytes length
    op, length = struct.unpack("!BI", header)
    if length > 0:
        payload = recv_exact(sock, length)
    else:
        payload = b""
    return op, payload


def main() -> None:
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <CID> <dest_filename>")
        sys.exit(1)

    cid = sys.argv[1]
    dest_name = sys.argv[2]

    # Resolve download_files directory next to this script (c_engine/)
    base_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(base_dir, "download_files")
    os.makedirs(out_dir, exist_ok=True)

    dest_path = os.path.join(out_dir, dest_name)

    print(f"[CLIENT] Connecting to {SOCK_PATH}")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(SOCK_PATH)

    try:
        # 1) Send DOWNLOAD_START with CID as payload
        print(f"[CLIENT] Sending DOWNLOAD_START for CID={cid}")
        send_frame(sock, OP_DOWNLOAD_START, cid.encode("ascii"))

        total_bytes = 0

        # 2) Open output file once and stream everything into it
        with open(dest_path, "wb") as f:
            while True:
                op, payload = recv_frame(sock)

                if op == OP_DOWNLOAD_CHUNK:
                    # Just write stream bytes directly, no chunk semantics on client
                    f.write(payload)
                    total_bytes += len(payload)

                elif op == OP_DOWNLOAD_DONE:
                    print("[CLIENT] DOWNLOAD_DONE received")
                    break

                else:
                    raise RuntimeError(
                        f"Unexpected opcode 0x{op:02x} while downloading"
                    )

    finally:
        sock.close()

    print(f"[CLIENT] Download finished. Saved to: {dest_path}")
    print(f"[CLIENT] Total bytes received = {total_bytes}")


if __name__ == "__main__":
    main()
