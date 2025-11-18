from pathlib import Path
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from fastapi.staticfiles import StaticFiles

BASE_DIR = Path(__file__).resolve().parent

app = FastAPI(
    title="IPFS Gateway",
    version="1.0.0",
    docs_url=None,
    redoc_url=None
)

app.mount("/static", StaticFiles(directory=str(BASE_DIR / "static")), name="static")
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))

@app.get("/", response_class=HTMLResponse)
async def root(request: Request):
    return templates.TemplateResponse("index.html", {"request": request})

@app.get("/docs", response_class=HTMLResponse)
async def docs(request: Request):
    return templates.TemplateResponse("docs.html", {"request": request})

@app.get("/try")
async def try_page():
    return "try page"
