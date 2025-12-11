from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent

ENGINE_SOCKET_PATH = "/tmp/cengine.sock"
GATEWAY_DATA_DIR = BASE_DIR / "data"
USERS_DB_PATH = GATEWAY_DATA_DIR / "users.json"
CHUNK_SIZE = 256 * 1024
SECRET_KEY = "simple-key"

GATEWAY_DATA_DIR.mkdir(parents=True, exist_ok=True)
