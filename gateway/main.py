from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from starlette.middleware.sessions import SessionMiddleware

from core.config import BASE_DIR, SECRET_KEY
from routers import auth, files, pages

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
