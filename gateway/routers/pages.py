from fastapi import APIRouter, Request
from fastapi.responses import HTMLResponse, RedirectResponse

from core.web import render_template

router = APIRouter()


@router.get("/", response_class=HTMLResponse)
async def landing(request: Request):
    if request.session.get("user"):
        return RedirectResponse(url="/dashboard", status_code=303)
    return render_template("index.html", request)


@router.get("/docs", response_class=HTMLResponse)
async def docs(request: Request):
    return render_template("docs.html", request)

