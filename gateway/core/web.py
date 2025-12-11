from typing import Any

from fastapi import Request
from fastapi.templating import Jinja2Templates

from .config import BASE_DIR

templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))


def render_template(name: str, request: Request, **context: Any):
    return templates.TemplateResponse(
        name,
        {
            "request": request,
            "user": request.session.get("user"),
            **context,
        },
    )

