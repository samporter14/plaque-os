"""Synthetic certificates only; private signing keys exist in memory, not files."""
from datetime import datetime, timezone
from pathlib import Path
import json
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

ROOT = Path(__file__).parent / "fixtures"
ROOT.mkdir(exist_ok=True)
def instant(text):
    return datetime.fromisoformat(text).replace(tzinfo=timezone.utc)
NOW = instant("2026-10-03T12:00:00")
START = instant("2025-01-01T00:00:00")
END = instant("2030-01-01T00:00:00")
def name(text):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, text)])
def make(subject, public_key, issuer, issuer_key, start, end, ca=False, dns=None):
    builder = (x509.CertificateBuilder().subject_name(name(subject))
        .issuer_name(name(issuer)).public_key(public_key).serial_number(x509.random_serial_number())
        .not_valid_before(start).not_valid_after(end)
        .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if ca:
        builder = builder.add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True)
    if dns:
        builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(dns)]), critical=False)
        builder = builder.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    return builder.sign(issuer_key, hashes.SHA256())
def save(filename, *certificates):
    (ROOT / filename).write_bytes(b"".join(c.public_bytes(serialization.Encoding.PEM) for c in certificates))

root_key = ec.generate_private_key(ec.SECP256R1())
root_name = "Synthetic Desk Notes Root"
root = make(root_name, root_key.public_key(), root_name, root_key, START, END, ca=True)
save("root.pem", root)
root_expired = make(root_name, root_key.public_key(), root_name, root_key, START, instant("2026-10-02T23:59:59"), ca=True)
save("expired-root.pem", root_expired)
leaf_key = ec.generate_private_key(ec.SECP256R1())
periods = {
    "valid": (instant("2026-10-01T00:00:00"), instant("2027-01-01T00:00:00")),
    "expired": (START, instant("2026-10-02T23:59:59")),
    "future": (instant("2026-10-04T00:00:00"), END),
    "boundary": (NOW, NOW),
}
for filename, (start, end) in periods.items():
    save(filename + ".pem", make("Synthetic notes.test", leaf_key.public_key(), root_name, root_key, start, end, dns="notes.test"))
save("wrong-host.pem", make("Synthetic wrong.test", leaf_key.public_key(), root_name, root_key, START, END, dns="wrong.test"))
rogue_key = ec.generate_private_key(ec.SECP256R1())
save("untrusted.pem", make("Synthetic notes.test", leaf_key.public_key(), "Synthetic Other Root", rogue_key, START, END, dns="notes.test"))
for state in ("expired", "future"):
    intermediate_key = ec.generate_private_key(ec.SECP256R1())
    subject = "Synthetic " + state + " intermediate"
    start, end = periods[state]
    intermediate = make(subject, intermediate_key.public_key(), root_name, root_key, start, end, ca=True)
    leaf = make("Synthetic notes.test", leaf_key.public_key(), subject, intermediate_key, START, END, dns="notes.test")
    save(state + "-intermediate.pem", leaf, intermediate)
valid_der = make("Synthetic notes.test", leaf_key.public_key(), root_name, root_key, START, END, dns="notes.test").public_bytes(serialization.Encoding.DER)
damaged = bytearray(valid_der)
damaged[-1] ^= 1
(ROOT / "bad-signature.der").write_bytes(damaged)
(ROOT / "clock.json").write_text(json.dumps({"epoch": int(NOW.timestamp()), "utc": NOW.isoformat(), "keys": "memory only"}, indent=2) + "\n")
print("Synthetic certificate fixtures created; UTC epoch", int(NOW.timestamp()))
