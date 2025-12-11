import anyio
from fastapi import APIRouter, Depends, File, HTTPException, Request, UploadFile, status
from fastapi.responses import RedirectResponse, StreamingResponse

from ..core.engine import engine_download_file, engine_list_files, engine_upload_bytes
from ..core.users import get_current_user
from ..core.web import render_template

router = APIRouter()


@router.get("/dashboard")
async def dashboard(request: Request, user=Depends(get_current_user)):
    try:
        files = await anyio.to_thread.run_sync(
            engine_list_files,
            user["auth_token"],
            cancellable=True,
        )
    except:
        files = []
    
    return render_template(
        "dashboard.html",
        request,
        files=files,
        message=request.query_params.get("msg"),
        error=request.query_params.get("err"),
        cid=request.query_params.get("cid"),
    )


@router.post("/upload")
async def upload_file(
    request: Request,
    user=Depends(get_current_user),
    file: UploadFile = File(...),
):
    content = await file.read()
    if not content:
        return RedirectResponse(
            url="/dashboard?err=Empty+file",
            status_code=status.HTTP_303_SEE_OTHER,
        )

    try:
        cid = await anyio.to_thread.run_sync(
            engine_upload_bytes,
            content,
            file.filename or "unnamed",
            user["auth_token"],
            cancellable=True,
        )
    except OSError:
        return RedirectResponse(
            url="/dashboard?err=Engine+unreachable",
            status_code=status.HTTP_303_SEE_OTHER,
        )

    if not cid:
        return RedirectResponse(
            url="/dashboard?err=Upload+failed",
            status_code=status.HTTP_303_SEE_OTHER,
        )

    return RedirectResponse(
        url=f"/dashboard?msg=Uploaded&cid={cid}",
        status_code=status.HTTP_303_SEE_OTHER,
    )


@router.get("/download/{cid}")
async def download(cid: str, user=Depends(get_current_user)):
    try:
        data, filename = await anyio.to_thread.run_sync(
            engine_download_file,
            cid,
            user["auth_token"],
            cancellable=True,
        )
    except OSError:
        raise HTTPException(status_code=502, detail="Engine unreachable")

    if data is None:
        raise HTTPException(status_code=404, detail="File not found")

    return StreamingResponse(
        iter([data]),
        media_type="application/octet-stream",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )

