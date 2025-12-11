import hashlib
import json
import time
from typing import Any, Dict

from fastapi import HTTPException, Request, status

from .config import USERS_DB_PATH


def load_users() -> Dict[str, Any]:
    if not USERS_DB_PATH.exists():
        return {}
    try:
        with open(USERS_DB_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_users(users: Dict[str, Any]) -> None:
    USERS_DB_PATH.write_text(json.dumps(users, indent=2), encoding="utf-8")


def make_password_hash(username: str, password: str) -> str:
    return hashlib.sha256(f"{username}:{password}".encode("utf-8")).hexdigest()


def make_auth_token(username: str, password: str) -> str:
    return "owner-" + make_password_hash(username, password)


def create_user(username: str, password: str) -> None:
    users = load_users()
    if username in users:
        raise ValueError("User already exists")
    users[username] = {
        "password_hash": make_password_hash(username, password),
        "created_at": time.time(),
    }
    save_users(users)


def verify_user(username: str, password: str) -> bool:
    users = load_users()
    if username not in users:
        return False
    return users[username]["password_hash"] == make_password_hash(username, password)


def get_current_user(request: Request) -> Dict[str, str]:
    username = request.session.get("user")
    auth_token = request.session.get("auth_token")
    if not username or not auth_token:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED)
    return {"username": username, "auth_token": auth_token}

