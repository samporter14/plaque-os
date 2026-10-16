from io import BytesIO
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from app import create_app
from demo_device import poll_once
from imaging import prepare

ORIGIN = "http://localhost:8787"
DEVICE_ID = "my-reader"
DEVICE_TOKEN = "test_device_" + "d" * 43
SENDER_SECRET = "test_sender_" + "s" * 43


class MailboxTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = dict(DEVICE_ID=DEVICE_ID, DEVICE_TOKEN=DEVICE_TOKEN, SENDER_SECRET=SENDER_SECRET,
            SESSION_SECRET="test_session_" + "c" * 43, ALLOW_LOCAL_HTTP="1", PUBLIC_ORIGIN=ORIGIN,
            MAILBOX_DB=str(Path(self.temp.name) / "mailbox.sqlite3"))
        self.app = create_app(self.settings)
        self.app.testing = True
        self.client = self.app.test_client()
        self.csrf = ""
        self.validator = os.environ.get("AMBIENT_PROTOCOL_CHECK")

    def tearDown(self):
        self.temp.cleanup()

    def get(self, path, device=False, **kwargs):
        return self.client.get(path, base_url=ORIGIN,
            headers={"Authorization": "Bearer " + DEVICE_TOKEN} if device else {}, **kwargs)

    def post(self, path, **kwargs):
        return self.client.post(path, base_url=ORIGIN,
            headers={"Origin": ORIGIN, "X-CSRF-Token": self.csrf}, **kwargs)

    def login(self):
        response = self.post("/api/sender/login", json={"secret": SENDER_SECRET})
        self.assertEqual(response.status_code, 200)
        self.csrf = response.json["csrf"]

    def send_text(self, text="hello"):
        preview = self.post("/api/sender/preview", data={"mode": "text", "text": text})
        self.assertEqual(preview.status_code, 200, preview.json)
        response = self.post("/api/sender/send", json={"draft_id": preview.json["draft_id"]})
        self.assertEqual(response.status_code, 200)
        return response.json, preview.json

    def metadata(self):
        return self.get(f"/api/device/{DEVICE_ID}/latest", device=True).json

    def test_private_reads_and_writes_require_correct_roles(self):
        self.assertEqual(self.get(f"/api/device/{DEVICE_ID}/latest").status_code, 401)
        self.assertEqual(self.get(f"/api/device/{DEVICE_ID}/image/m1").status_code, 401)
        self.assertEqual(self.post("/api/sender/preview", data={"mode": "text", "text": "hello"}).status_code, 401)
        self.assertEqual(self.client.get(f"/api/device/{DEVICE_ID}/latest", base_url=ORIGIN,
            headers={"Authorization": "Bearer " + SENDER_SECRET}).status_code, 401)
        self.assertEqual(self.post("/api/sender/login", json={"secret": "♥"}).status_code, 401)

    def test_empty_mailbox(self):
        self.assertEqual(self.metadata(), {"message_id": None})

    def test_text_preview_and_payload_are_identical_monochrome(self):
        self.login()
        _, preview = self.send_text()
        png = self.get(preview["preview_url"])
        # Sending consumes the private draft, so a stale preview is no longer readable.
        self.assertEqual(png.status_code, 404)
        meta = self.metadata()
        payload = self.get(meta["image_url"], device=True)
        self.assertEqual(payload.status_code, 200)
        self.assertEqual(len(payload.data), 48000)
        self.assertEqual(hashlib.sha256(payload.data).hexdigest(), meta["sha256"])
        self.assertEqual(payload.headers["Cache-Control"], "no-store")
        self.assertEqual(Image.frombytes("1", (800, 480), payload.data).size, (800, 480))

    def test_preview_matches_sent_bytes(self):
        self.login()
        preview = self.post("/api/sender/preview", data={"mode": "text", "text": "hello"}).json
        png = self.get(preview["preview_url"]).data
        image = Image.open(BytesIO(png))
        self.assertEqual(set(image.get_flattened_data()), {0, 255})
        self.post("/api/sender/send", json={"draft_id": preview["draft_id"]})
        payload = self.get(self.metadata()["image_url"], device=True).data
        self.assertEqual(image.convert("1").tobytes(), payload)

    def test_doodle_exact_dimensions_and_pixel_polarity(self):
        image = Image.new("RGB", (800, 480), "white")
        image.putpixel((0, 0), (0, 0, 0))
        source = BytesIO(); image.save(source, "PNG")
        payload, _ = prepare("draw", data=source.getvalue())
        self.assertEqual(payload[0], 0x7F)
        self.assertEqual(payload[1:], bytes([255]) * 47999)
        with self.assertRaises(ValueError):
            other = BytesIO(); Image.new("RGB", (20, 20)).save(other, "PNG")
            prepare("draw", data=other.getvalue())

    def test_photo_fit_and_crop(self):
        self.login()
        source = BytesIO(); Image.new("RGB", (120, 240), "black").save(source, "PNG")
        for fit in ("fit", "crop"):
            preview = self.post("/api/sender/preview", data={"mode": "photo", "fit": fit,
                "image": (BytesIO(source.getvalue()), "photo.png")})
            self.assertEqual(preview.status_code, 200)
            self.post("/api/sender/send", json={"draft_id": preview.json["draft_id"]})
            payload = self.get(self.metadata()["image_url"], device=True).data
            image = Image.frombytes("1", (800, 480), payload)
            self.assertEqual(image.getpixel((0, 0)), 255 if fit == "fit" else 0)

    def test_versions_atomic_immutable_and_bounded(self):
        self.login()
        self.send_text("one"); first = self.metadata()
        first_payload = self.get(first["image_url"], device=True).data
        self.send_text("two"); second = self.metadata()
        self.assertEqual(self.get(first["image_url"], device=True).data, first_payload)
        self.send_text("three"); third = self.metadata()
        self.assertEqual([first["message_id"], second["message_id"], third["message_id"]], ["m1", "m2", "m3"])
        self.assertEqual(self.get(first["image_url"], device=True).status_code, 410)
        restarted = create_app(self.settings).test_client()
        current = restarted.get(f"/api/device/{DEVICE_ID}/latest", base_url=ORIGIN,
            headers={"Authorization": "Bearer " + DEVICE_TOKEN})
        self.assertEqual(current.json["message_id"], "m3")

    def test_send_replay_is_rejected(self):
        self.login()
        sent, preview = self.send_text()
        repeated = self.post("/api/sender/send", json={"draft_id": preview["draft_id"]})
        self.assertEqual(repeated.status_code, 409)
        self.assertEqual(self.metadata()["message_id"], sent["message_id"])

    def test_csrf_and_origin(self):
        self.login()
        response = self.client.post("/api/sender/preview", base_url=ORIGIN,
            headers={"Origin": ORIGIN}, data={"mode": "text", "text": "bad"})
        self.assertEqual(response.status_code, 403)
        response = self.client.post("/api/sender/preview", base_url=ORIGIN,
            headers={"Origin": "https://evil.example", "X-CSRF-Token": self.csrf}, data={"mode": "text", "text": "bad"})
        self.assertEqual(response.status_code, 403)

    def test_preview_not_shared_between_sender_sessions(self):
        self.login()
        preview = self.post("/api/sender/preview", data={"mode": "text", "text": "private"}).json
        other = self.app.test_client()
        other.post("/api/sender/login", base_url=ORIGIN, headers={"Origin": ORIGIN}, json={"secret": SENDER_SECRET})
        self.assertEqual(other.get(preview["preview_url"], base_url=ORIGIN).status_code, 404)

    def test_logout_and_invalid_upload(self):
        self.login()
        bad = self.post("/api/sender/preview", data={"mode": "photo", "image": (BytesIO(b"not-an-image"), "x.png")})
        self.assertEqual(bad.status_code, 400)
        self.assertEqual(self.post("/api/sender/logout").status_code, 200)
        self.assertEqual(self.get("/api/sender/session").status_code, 401)

    def test_deployment_requires_https_and_distinct_secrets(self):
        config = {**self.settings, "ALLOW_LOCAL_HTTP": "0"}
        with self.assertRaises(ValueError): create_app(config)
        with self.assertRaises(ValueError): create_app({**self.settings, "SENDER_SECRET": DEVICE_TOKEN})

    def test_local_device_can_use_lan_host_but_sender_must_use_origin(self):
        self.assertEqual(self.client.get(f"/api/device/{DEVICE_ID}/latest", base_url="http://192.168.1.10:8787",
            headers={"Authorization": "Bearer " + DEVICE_TOKEN}).status_code, 200)
        self.assertEqual(self.client.get("/", base_url="http://wrong.example:8787").status_code, 400)

    def test_polling_new_unchanged_failures_and_cache_repair(self):
        self.login(); self.send_text()
        state_dir = Path(self.temp.name) / "device"
        requests = []
        def fetch(path, limit):
            requests.append(path)
            result = self.get(path, device=True)
            if result.status_code != 200: raise OSError("network error")
            return result.data
        poll = lambda **kwargs: poll_once(ORIGIN, DEVICE_ID, DEVICE_TOKEN, state_dir, self.validator, **kwargs)
        self.assertEqual(poll(get=fetch), "updated")
        first = (state_dir / "frame.bin").read_bytes()
        png_time = (state_dir / "frame.png").stat().st_mtime_ns
        requests.clear()
        self.assertEqual(poll(get=fetch), "unchanged")
        self.assertEqual(len(requests), 1)
        self.assertEqual((state_dir / "frame.png").stat().st_mtime_ns, png_time)
        self.assertEqual(json.loads((state_dir / "state.json").read_text())["refreshes"], 1)
        def broken(path, limit): raise OSError("offline")
        self.assertEqual(poll(get=broken), "failed")
        self.assertEqual((state_dir / "frame.bin").read_bytes(), first)
        self.send_text("a new note")
        def corrupt(path, limit):
            data = fetch(path, limit)
            return data if path.endswith("/latest") else data[:-1]
        self.assertEqual(poll(get=corrupt), "failed")
        self.assertEqual((state_dir / "frame.bin").read_bytes(), first)
        self.assertEqual(poll(get=fetch), "updated")
        (state_dir / "frame.bin").write_bytes(b"broken cache")
        self.assertEqual(poll(get=fetch), "updated")


if __name__ == "__main__":
    unittest.main()
