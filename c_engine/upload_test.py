import socket
import struct
import sys
import os

SOCK_PATH = "/tmp/cengine.sock"

OP_UPLOAD_START  = 0x01
OP_UPLOAD_CHUNK  = 0x02
OP_UPLOAD_FINISH = 0x03
OP_UPLOAD_DONE   = 0x81

def send_frame(sock, op, payload: bytes):
    if payload is None:
        payload = b""
    header = struct.pack("!BI", op, len(payload))  # 1 byte op, 4 bytes len (big-endian)
    sock.sendall(header + payload)

def recv_frame(sock):
    # read header: 1 byte op + 4 bytes len
    header = sock.recv(5)
    if len(header) == 0:
        return None, None
    if len(header) < 5:
        raise RuntimeError("short header")
    op, length = struct.unpack("!BI", header)
    # read payload
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("connection closed while reading payload")
        data += chunk
    return op, data

def upload_file(path: str):
    # فقط اسم فایل، بدون مسیر، رو به عنوان filename می‌فرستیم
    filename = os.path.basename(path).encode("utf-8")

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(SOCK_PATH)

    print(f"[CLIENT] Connected to {SOCK_PATH}")
    # 1) UPLOAD_START با filename
    print(f"[CLIENT] Sending UPLOAD_START filename={filename.decode('utf-8')!r}")
    send_frame(sock, OP_UPLOAD_START, filename)

    # 2) UPLOAD_CHUNK ها — فعلاً chunking سمت Pythonه (C فقط هر chunk را به عنوان بلاک می‌گیرد)
    chunk_size = 64 * 1024  # 64KB فقط برای تست؛ ENGINE_CHUNK_SIZE سمت C هنوز مهم نیست اینجا
    total = 0
    with open(path, "rb") as f:
        while True:
            buf = f.read(chunk_size)
            if not buf:
                break
            total += len(buf)
            print(f"[CLIENT] Sending UPLOAD_CHUNK len={len(buf)} total_sent={total}")
            send_frame(sock, OP_UPLOAD_CHUNK, buf)

    # 3) UPLOAD_FINISH
    print("[CLIENT] Sending UPLOAD_FINISH")
    send_frame(sock, OP_UPLOAD_FINISH, b"")

    # 4) دریافت پاسخ CID
    op, payload = recv_frame(sock)
    if op != OP_UPLOAD_DONE:
        raise RuntimeError(f"expected OP_UPLOAD_DONE (0x81), got 0x{op:02x}")
    cid = payload.decode("utf-8")
    print(f"[CLIENT] Got CID from engine: {cid}")

    sock.close()
    return cid

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: python3 {sys.argv[0]} <path-to-file>")
        sys.exit(1)
    path = sys.argv[1]
    cid = upload_file(path)
    print(f"[CLIENT] Upload finished, CID = {cid}")
