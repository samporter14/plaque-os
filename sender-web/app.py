"""Single-device private mailbox; SQLite provides atomic version/payload publication."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
from functools import wraps
import hashlib
import hmac
from io import BytesIO
import os
from pathlib import Path
import re
import secrets
import sqlite3
import threading
import time
from urllib.parse import urlsplit

from flask import Flask, abort, jsonify, request, send_file, session
from werkzeug.exceptions import HTTPException

from imaging import FORMAT, HEIGHT, PAYLOAD_BYTES, WIDTH, prepare

ROOT = Path(__file__).resolve().parent


def same_secret(left, right):
    return hmac.compare_digest(left.encode("utf-8"), right.encode("utf-8"))


def read_env(path=ROOT / ".env"):
    result = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if line.strip() and not line.lstrip().startswith("#") and "=" in line:
                name, value = line.split("=", 1)
                result[name.strip()] = value.strip()
    result.update({k: os.environ[k] for k in (
        "DEVICE_ID", "DEVICE_TOKEN", "SENDER_SECRET", "SESSION_SECRET", "PUBLIC_ORIGIN",
        "ALLOW_LOCAL_HTTP", "MAILBOX_DB") if k in os.environ})
    return result


def create_app(settings=None):
    config = read_env() if settings is None else settings
    for key in ("DEVICE_TOKEN", "SENDER_SECRET", "SESSION_SECRET"):
        if len(config.get(key, "")) < 32 or config[key].startswith("REPLACE_"):
            raise ValueError(f"Set a random secret for {key}; run setup_local.py for a local demo.")
    if len({config[key] for key in ("DEVICE_TOKEN", "SENDER_SECRET", "SESSION_SECRET")}) != 3:
        raise ValueError("Use three different secrets for device, sender, and session signing.")
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", config["DEVICE_TOKEN"]):
        raise ValueError("DEVICE_TOKEN must be a random URL-safe secret, 32–128 characters.")
    device_id = config.get("DEVICE_ID", "my-reader")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", device_id):
        raise ValueError("DEVICE_ID must be 1–32 letters, digits, underscores or hyphens.")
    origin = config.get("PUBLIC_ORIGIN", "http://localhost:8787").rstrip("/")
    parsed_origin = urlsplit(origin)
    local_http = str(config.get("ALLOW_LOCAL_HTTP", "0")) == "1"
    if not parsed_origin.netloc or parsed_origin.path or parsed_origin.query or parsed_origin.fragment or \
       parsed_origin.username or parsed_origin.scheme not in ("https", "http"):
        raise ValueError("PUBLIC_ORIGIN must be a single HTTP(S) origin, without a path.")
    if parsed_origin.scheme != "https" and not local_http:
        raise ValueError("Deployed configurations require an HTTPS PUBLIC_ORIGIN.")
    if local_http and (parsed_origin.scheme != "http" or parsed_origin.hostname not in ("localhost", "127.0.0.1")):
        raise ValueError("The HTTP sender demo uses localhost. Use HTTPS for remote senders.")
    app = Flask(__name__, static_folder=str(ROOT / "static"), static_url_path="")
    app.config.update(SECRET_KEY=config["SESSION_SECRET"], MAX_CONTENT_LENGTH=12 * 1024 * 1024,
                      SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE="Strict",
                      SESSION_COOKIE_SECURE=not local_http, PERMANENT_SESSION_LIFETIME=3600)
    db_path = Path(config.get("MAILBOX_DB", ".data/mailbox.sqlite3"))
    if not db_path.is_absolute():
        db_path = ROOT / db_path
    db_path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(db_path) as db:
        db.executescript("""
            CREATE TABLE IF NOT EXISTS messages (
                version INTEGER PRIMARY KEY AUTOINCREMENT, created_at TEXT NOT NULL,
                payload BLOB NOT NULL, preview BLOB NOT NULL, sha256 TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS drafts (
                id TEXT PRIMARY KEY, owner TEXT NOT NULL, expires REAL NOT NULL,
                payload BLOB NOT NULL, preview BLOB NOT NULL);
        """)
    db_path.chmod(0o600)
    attempts, attempt_lock = {}, threading.Lock()

    @contextmanager
    def connect():
        db = sqlite3.connect(db_path, timeout=5)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()

    def sender_required(fn):
        @wraps(fn)
        def wrapped(*args, **kwargs):
            if not session.get("sender"):
                abort(401, "Sign in to send a note.")
            if request.method != "GET" and not same_secret(
                request.headers.get("X-CSRF-Token", ""), session.get("csrf", "")):
                abort(403, "Your session changed. Please sign in again.")
            return fn(*args, **kwargs)
        return wrapped

    def device_required(fn):
        @wraps(fn)
        def wrapped(device, *args, **kwargs):
            if device != device_id or not same_secret(
                request.headers.get("Authorization", ""), "Bearer " + config["DEVICE_TOKEN"]):
                abort(401, "Device authentication required.")
            return fn(device, *args, **kwargs)
        return wrapped

    @app.before_request
    def protect_origin():
        if request.host != parsed_origin.netloc and not (local_http and request.path.startswith("/api/device/")):
            abort(400, "Use the configured mailbox address.")
        # A reverse proxy must terminate TLS and deny direct public access to this service.
        if request.method not in ("GET", "HEAD", "OPTIONS") and request.headers.get("Origin") != origin:
            abort(403, "Requests must come from this sender page.")

    @app.after_request
    def private_response(response):
        response.headers["Cache-Control"] = "no-store" if request.path.startswith("/api/") else "no-cache"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' blob: data:; "
            "connect-src 'self'; object-src 'none'; base-uri 'self'; frame-ancestors 'none'")
        if not local_http:
            response.headers["Strict-Transport-Security"] = "max-age=31536000"
        return response

    @app.errorhandler(HTTPException)
    def api_error(error):
        return jsonify(error=error.description), error.code

    @app.get("/")
    def index():
        return app.send_static_file("index.html")

    @app.post("/api/sender/login")
    def login():
        now = time.monotonic()
        key = request.remote_addr or "local"
        with attempt_lock:
            # Drop old entries so repeated invalid requests cannot grow RAM forever.
            for old_key in list(attempts):
                if now - attempts[old_key][0] > 300:
                    del attempts[old_key]
            if len(attempts) >= 256 and key not in attempts:
                abort(429, "Please try again in a few minutes.")
            start, count = attempts.get(key, (now, 0))
            if count >= 10:
                abort(429, "Please try again in a few minutes.")
            attempts[key] = (start, count + 1)
        body = request.get_json(silent=True) or {}
        secret = body.get("secret", "") if isinstance(body, dict) else ""
        if not isinstance(secret, str) or not same_secret(secret, config["SENDER_SECRET"]):
            abort(401, "That secret did not match.")
        with attempt_lock:
            attempts.pop(key, None)
        session.clear()
        session.update(sender=True, owner=secrets.token_urlsafe(24), csrf=secrets.token_urlsafe(32))
        session.permanent = True
        return jsonify(csrf=session["csrf"], device_id=device_id)

    @app.get("/api/sender/session")
    @sender_required
    def current_session():
        return jsonify(csrf=session["csrf"], device_id=device_id)

    @app.post("/api/sender/logout")
    @sender_required
    def logout():
        with connect() as db:
            db.execute("DELETE FROM drafts WHERE owner=?", (session["owner"],))
        session.clear()
        return jsonify(ok=True)

    @app.post("/api/sender/preview")
    @sender_required
    def preview():
        mode = request.form.get("mode", "")
        uploaded = request.files.get("image")
        data = uploaded.read() if uploaded else b""
        try:
            payload, png = prepare(mode, request.form.get("text", ""), data, request.form.get("fit", "fit"))
        except ValueError as exc:
            abort(400, str(exc))
        draft_id = secrets.token_urlsafe(24)
        with connect() as db:
            db.execute("DELETE FROM drafts WHERE expires < ? OR owner=?", (time.time(), session["owner"]))
            count = db.execute("SELECT COUNT(*) FROM drafts").fetchone()[0]
            if count >= 64:
                abort(429, "The mailbox is busy. Try again in a minute.")
            db.execute("INSERT INTO drafts VALUES (?, ?, ?, ?, ?)",
                       (draft_id, session["owner"], time.time() + 600, payload, png))
        return jsonify(draft_id=draft_id, preview_url=f"/api/sender/draft/{draft_id}/preview",
                       width=WIDTH, height=HEIGHT, byte_length=PAYLOAD_BYTES)

    @app.get("/api/sender/draft/<draft_id>/preview")
    @sender_required
    def draft_preview(draft_id):
        with connect() as db:
            row = db.execute("SELECT preview FROM drafts WHERE id=? AND owner=? AND expires>?",
                             (draft_id, session["owner"], time.time())).fetchone()
        if row is None:
            abort(404, "Preview expired. Create a fresh preview.")
        return send_file(BytesIO(row["preview"]), mimetype="image/png", max_age=0)

    @app.post("/api/sender/send")
    @sender_required
    def send():
        body = request.get_json(silent=True) or {}
        draft_id = body.get("draft_id", "") if isinstance(body, dict) else ""
        if not isinstance(draft_id, str):
            abort(400, "Create a preview before sending.")
        with connect() as db:
            db.execute("BEGIN IMMEDIATE")
            draft = db.execute("SELECT * FROM drafts WHERE id=? AND owner=? AND expires>?",
                               (draft_id, session["owner"], time.time())).fetchone()
            if draft is None:
                abort(409, "Preview expired or already sent. Create a fresh preview.")
            created = datetime.now(timezone.utc).isoformat(timespec="seconds")
            digest = hashlib.sha256(draft["payload"]).hexdigest()
            cursor = db.execute("INSERT INTO messages(created_at,payload,preview,sha256) VALUES(?,?,?,?)",
                                (created, draft["payload"], draft["preview"], digest))
            version = cursor.lastrowid
            db.execute("DELETE FROM drafts WHERE id=?", (draft_id,))
            # Retain one previous immutable version for a metadata/download race.
            db.execute("DELETE FROM messages WHERE version NOT IN (SELECT version FROM messages ORDER BY version DESC LIMIT 2)")
        return jsonify(message_id=f"m{version}", created_at=created)

    @app.get("/api/device/<device>/latest")
    @device_required
    def latest(device):
        with connect() as db:
            row = db.execute("SELECT version, created_at, sha256 FROM messages ORDER BY version DESC LIMIT 1").fetchone()
        if row is None:
            return jsonify(message_id=None)
        message_id = f"m{row['version']}"
        response = jsonify(message_id=message_id, created_at=row["created_at"], sha256=row["sha256"],
            image_url=f"/api/device/{device}/image/{message_id}", format=FORMAT,
            width=WIDTH, height=HEIGHT, byte_length=PAYLOAD_BYTES)
        response.headers["ETag"] = f'"{message_id}"'
        return response

    @app.get("/api/device/<device>/image/<message_id>")
    @device_required
    def image(device, message_id):
        if not re.fullmatch(r"m[1-9][0-9]{0,18}", message_id):
            abort(404, "Message not found.")
        with connect() as db:
            row = db.execute("SELECT payload FROM messages WHERE version=?", (int(message_id[1:]),)).fetchone()
        if row is None:
            abort(410, "This version expired; check the latest metadata again.")
        return send_file(BytesIO(row["payload"]), mimetype="application/octet-stream", max_age=0)

    return app


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    application = create_app()
    application.run(host=args.host, port=args.port, debug=False, use_reloader=False)


if __name__ == "__main__":
    main()
