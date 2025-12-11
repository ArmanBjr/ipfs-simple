from fastapi import APIRouter, Form, Request, status
from fastapi.responses import HTMLResponse, RedirectResponse

from core.users import create_user, get_current_user, make_auth_token, verify_user
from core.web import render_template

router = APIRouter()


@router.get("/signin", response_class=HTMLResponse)
async def signin_form(request: Request):
    if request.session.get("user"):
        return RedirectResponse(url="/dashboard", status_code=status.HTTP_303_SEE_OTHER)
    return render_template("auth.html", request, mode="signin")


@router.post("/signin")
async def signin(request: Request, username: str = Form(...), password: str = Form(...)):
    if not verify_user(username, password):
        return render_template(
            "auth.html",
            request,
            mode="signin",
            error="Invalid username or password",
        )

    request.session["user"] = username
    request.session["auth_token"] = make_auth_token(username, password)
    return RedirectResponse(url="/dashboard", status_code=status.HTTP_303_SEE_OTHER)


@router.get("/signup", response_class=HTMLResponse)
async def signup_form(request: Request):
    if request.session.get("user"):
        return RedirectResponse(url="/dashboard", status_code=status.HTTP_303_SEE_OTHER)
    return render_template("auth.html", request, mode="signup")


@router.post("/signup")
async def signup(
    request: Request,
    username: str = Form(...),
    password: str = Form(...),
):
    try:
        create_user(username, password)
    except ValueError as e:
        return render_template(
            "auth.html",
            request,
            mode="signup",
            error=str(e),
        )
    request.session["user"] = username
    request.session["auth_token"] = make_auth_token(username, password)
    return RedirectResponse(url="/dashboard", status_code=status.HTTP_303_SEE_OTHER)


@router.get("/logout")
async def logout(request: Request):
    request.session.clear()
    return RedirectResponse(url="/", status_code=status.HTTP_303_SEE_OTHER)

