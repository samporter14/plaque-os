"""Create private, ignored local configuration. Never prints device/session tokens."""
import json
from pathlib import Path
import secrets

ROOT = Path(__file__).resolve().parent


def main():
    path = ROOT / ".env"
    if path.exists():
        raise SystemExit(".env already exists; preserving it. See README.md to change settings.")
    device_token, sender_secret, session_secret = [secrets.token_urlsafe(32) for _ in range(3)]
    path.write_text(
        f"DEVICE_ID=my-reader\nDEVICE_TOKEN={device_token}\nSENDER_SECRET={sender_secret}\n"
        f"SESSION_SECRET={session_secret}\nALLOW_LOCAL_HTTP=1\n"
        "PUBLIC_ORIGIN=http://localhost:8787\nMAILBOX_DB=.data/mailbox.sqlite3\n"
    )
    path.chmod(0o600)
    config_path = ROOT / "device-config.local.json"
    config_path.write_text(json.dumps({
        "base_url": "http://192.168.1.10:8787",
        "device_id": "my-reader", "device_token": device_token, "poll_minutes": 60,
        "use_as_sleep_screen": False, "scheduled_polling": False, "allow_local_http": True,
    }, indent=2) + "\n")
    config_path.chmod(0o600)
    print("Created private .env and device-config.local.json. All timed polling starts disabled.")
    print("Open .env locally to copy SENDER_SECRET into the sender's login field.")
    print("The device-config file is a future SD configuration template; replace its example LAN address.")


if __name__ == "__main__":
    main()
