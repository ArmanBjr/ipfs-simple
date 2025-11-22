#!/usr/bin/env python3
import socket
import struct

SOCK_PATH = "/tmp/cengine.sock"

# Opcodes (must match c_engine.c)
OP_UPLOAD_START  = 0x01
OP_UPLOAD_CHUNK  = 0x02
OP_UPLOAD_FINISH = 0x03
OP_UPLOAD_DONE   = 0x81

def send_frame(sock, op, payload: bytes):
    header = struct.pack("!BI", op, len(payload))
    sock.sendall(header + payload)

def recv_frame(sock):
    # Read 1 + 4 bytes header
    hdr = sock.recv(5)
    if not hdr:
        return None, None
    op, length = struct.unpack("!BI", hdr)
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            break
        payload += chunk
    return op, payload

def test_upload():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(SOCK_PATH)
    print("[CLIENT] connected")

    # 1) UPLOAD_START with filename "test.txt"
    filename = "test.txt".encode("utf-8")
    print("[CLIENT] sending UPLOAD_START")
    send_frame(sock, OP_UPLOAD_START, filename)

    # 2) First CHUNK: "AAAAA..."
    data1 = b"A" * 10
    print("[CLIENT] sending UPLOAD_CHUNK len", len(data1))
    send_frame(sock, OP_UPLOAD_CHUNK, data1)

    # 3) Second CHUNK: "BBBBB..."
    data2 = b"B" * 15
    print("[CLIENT] sending UPLOAD_CHUNK len", len(data2))
    send_frame(sock, OP_UPLOAD_CHUNK, data2)

    # 4) FINISH
    print("[CLIENT] sending UPLOAD_FINISH")
    send_frame(sock, OP_UPLOAD_FINISH, b"")

    # 5) Wait for UPLOAD_DONE
    op, payload = recv_frame(sock)
    print("[CLIENT] got frame: op=0x%02x payload=%r" % (op, payload))

    sock.close()
    print("[CLIENT] closed connection")

if __name__ == "__main__":
    test_upload()
