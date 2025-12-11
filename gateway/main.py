import sys
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from starlette.middleware.sessions import SessionMiddleware

# Allow running as a script (uvicorn main:app from gateway dir) or as package (gateway.main)
CURRENT_DIR = Path(__file__).resolve().parent
PARENT_DIR = CURRENT_DIR.parent
if str(PARENT_DIR) not in sys.path:
    sys.path.insert(0, str(PARENT_DIR))

from gateway.core.config import BASE_DIR, SECRET_KEY  # type: ignore
from gateway.routers import auth, files, pages  # type: ignore

app = FastAPI(
    title="IPFS Gateway",
    version="1.0.0",
    docs_url=None,
    redoc_url=None,
)

app.add_middleware(SessionMiddleware, secret_key=SECRET_KEY)
app.mount("/static", StaticFiles(directory=str(BASE_DIR / "static")), name="static")

app.include_router(pages.router)
app.include_router(auth.router)
app.include_router(files.router)
