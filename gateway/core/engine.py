import json
import socket
import struct
from typing import List, Dict, Any, Optional, Tuple

from .config import CHUNK_SIZE, ENGINE_SOCKET_PATH

OP_UPLOAD_START = 0x01
OP_UPLOAD_CHUNK = 0x02
OP_UPLOAD_FINISH = 0x03
OP_UPLOAD_RESUME = 0x04
OP_AUTH = 0x05
OP_UPLOAD_DONE = 0x81
OP_DELETE_FILE = 0x20

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE = 0x92

OP_LIST_FILES = 0x30
OP_LIST_RESPONSE = 0xB0


def recvn(sock: socket.socket, n: int) -> Optional[bytes]:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            if len(data) == 0:
                return None
            raise IOError("Unexpected EOF from socket")
        data.extend(chunk)
    return bytes(data)


def send_frame(sock: socket.socket, op: int, payload: bytes = b"") -> None:
    payload = payload or b""
    length = len(payload)
    header = struct.pack("!BI", op, length)
    sock.sendall(header)
    if length > 0:
        sock.sendall(payload)


def recv_frame(sock: socket.socket) -> Tuple[Optional[int], Optional[bytes]]:
    header = recvn(sock, 5)
    if header is None:
        return None, None
    op, length = struct.unpack("!BI", header)
    payload = recvn(sock, length) if length > 0 else b""
    if length > 0 and payload is None:
        raise IOError("Unexpected EOF while reading payload")
    return op, payload


def engine_upload_bytes(data: bytes, filename: str, auth_token: str) -> Optional[str]:
    total_size = len(data)
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect(ENGINE_SOCKET_PATH)
    except (OSError, socket.timeout) as e:
        raise OSError(f"Cannot connect to engine: {e}")

    try:
        send_frame(sock, OP_AUTH, auth_token.encode("utf-8"))
        payload = struct.pack("!Q", total_size) + filename.encode("utf-8")
        send_frame(sock, OP_UPLOAD_START, payload)

        offset = 0
        while offset < total_size:
            chunk = data[offset : offset + CHUNK_SIZE]
            offset += len(chunk)
            send_frame(sock, OP_UPLOAD_CHUNK, chunk)

        send_frame(sock, OP_UPLOAD_FINISH, b"")

        cid = None
        while True:
            op, payload = recv_frame(sock)
            if op is None:
                break
            if op == OP_UPLOAD_DONE:
                text = payload.decode("utf-8") if payload else ""
                if text not in ("UPLOAD_ERROR", "AUTH_ERROR", ""):
                    cid = text.strip()
                break
        return cid
    finally:
        sock.close()


def engine_download_file(cid: str, auth_token: str) -> Tuple[Optional[bytes], Optional[str]]:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect(ENGINE_SOCKET_PATH)
    except (OSError, socket.timeout) as e:
        raise OSError(f"Cannot connect to engine: {e}")

    try:
        send_frame(sock, OP_AUTH, auth_token.encode("utf-8"))
        send_frame(sock, OP_DOWNLOAD_START, cid.encode("utf-8"))

        data = bytearray()
        filename = cid
        
        while True:
            op, payload = recv_frame(sock)
            if op is None:
                return None, None
            if op == OP_DOWNLOAD_CHUNK:
                data.extend(payload)
            elif op == OP_DOWNLOAD_DONE:
                text = payload.decode("utf-8") if payload else ""
                if text in ("DOWNLOAD_ERROR", "AUTH_ERROR"):
                    return None, None
                filename = text if text else cid
                break

        return bytes(data), filename
    finally:
        sock.close()


def engine_list_files(auth_token: str) -> List[Dict[str, Any]]:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect(ENGINE_SOCKET_PATH)
    except (OSError, socket.timeout) as e:
        raise OSError(f"Cannot connect to engine: {e}")

    try:
        send_frame(sock, OP_AUTH, auth_token.encode("utf-8"))
        send_frame(sock, OP_LIST_FILES, b"")

        while True:
            op, payload = recv_frame(sock)
            if op is None:
                return []
            if op == OP_LIST_RESPONSE:
                if not payload:
                    return []
                try:
                    files = json.loads(payload.decode("utf-8"))
                    return files if isinstance(files, list) else []
                except:
                    return []
        
        return []
    finally:
        sock.close()
