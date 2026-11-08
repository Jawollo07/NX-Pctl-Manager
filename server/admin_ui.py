"""Same-origin browser dashboard for the backend administration API."""
from pathlib import Path

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse

router = APIRouter()
_STATIC = Path(__file__).resolve().parent / "static"
_ASSETS = {"admin.css": "text/css; charset=utf-8", "admin.js": "text/javascript; charset=utf-8"}


@router.get("/admin", include_in_schema=False)
@router.get("/admin/", include_in_schema=False)
def admin_dashboard():
    response = FileResponse(_STATIC / "admin.html", media_type="text/html; charset=utf-8")
    response.headers.update({
        "Cache-Control": "no-store",
        "Content-Security-Policy": "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
        "X-Content-Type-Options": "nosniff",
        "Referrer-Policy": "no-referrer",
        "X-Frame-Options": "DENY",
    })
    return response


@router.get("/admin/assets/{asset_name}", include_in_schema=False)
def admin_asset(asset_name: str):
    if asset_name not in _ASSETS:
        raise HTTPException(404, "asset not found")
    response = FileResponse(_STATIC / asset_name, media_type=_ASSETS[asset_name])
    response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    return response
