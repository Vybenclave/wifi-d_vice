#!/usr/bin/env python3
"""tools/export_job.py -- decrypt a pulled client folder's logs and bundle
them into one plain zip for easy off-device use.

Format this decrypts (verified against wifi_d_vice/wlog.cpp, engstore.cpp,
crypto.cpp -- do not change without re-checking those):
  - Each *.csv under <client_dir> has 3 plaintext header lines, then one
    record per data row:
      line 1: "client=...;tester=...;armed_at=..." (the GCM AAD for every
               row in this file, used verbatim, never reconstructed)
      line 2: "encrypted=aes256gcm-hex" or "encrypted=none" (always one or
               the other -- never absent)
      line 3: the CSV column header
    An encrypted row is hex([12-byte nonce][ciphertext][16-byte GCM tag]).
    A row can also be the literal marker "ENC_FAIL" (the firmware's own
    "armed but this row's encrypt failed, dropped" marker) -- not hex, not
    a corrupt row, just skip it with its own distinct warning.
  - <client_dir>/eng.txt has plaintext k=v lines "salt"/"iters"/"verifier"
    (hex). verifier = AES-256-GCM(key, aad=salt, plaintext=
    "VYBEN-ENG-UNLOCK-v1"), hex-encoded -- a good tag on that exact string
    is how a passphrase is confirmed correct, matching the firmware's own
    engStoreUnlockCheck().
  - Key = PBKDF2-HMAC-SHA256(passphrase, salt, iters, dklen=32).
"""

import argparse
import sys
import zipfile
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

EXPECTED_VERIFIER_PLAINTEXT = b"VYBEN-ENG-UNLOCK-v1"
ENCRYPTED_MARKER = b"encrypted=aes256gcm-hex"
ENC_FAIL_MARKER = b"ENC_FAIL"
NONCE_LEN = 12
TAG_LEN = 16


def read_eng_txt(client_dir: Path) -> dict:
    eng_path = client_dir / "eng.txt"
    if not eng_path.is_file():
        print(f"error: {eng_path} not found", file=sys.stderr)
        sys.exit(1)
    data = {}
    with open(eng_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            key, _, value = line.partition("=")
            data[key.strip()] = value.strip()
    return data


def derive_key(passphrase: str, salt: str, iters: int) -> bytes:
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt.encode("utf-8"),
        iterations=iters,
    )
    return kdf.derive(passphrase.encode("utf-8"))


def gcm_open(key: bytes, blob: bytes, aad: bytes):
    """blob = nonce(12) + ciphertext + tag(16). Returns plaintext, or None
    on a bad tag / malformed blob -- never raises out of this function."""
    if len(blob) < NONCE_LEN + TAG_LEN:
        return None
    nonce = blob[:NONCE_LEN]
    ciphertext_and_tag = blob[NONCE_LEN:]   # AESGCM wants ciphertext+tag combined, not split
    try:
        return AESGCM(key).decrypt(nonce, ciphertext_and_tag, aad)
    except Exception:
        return None


def verify_passphrase(key: bytes, verifier_hex: str, salt: str) -> bool:
    try:
        blob = bytes.fromhex(verifier_hex)
    except ValueError:
        return False
    plaintext = gcm_open(key, blob, salt.encode("utf-8"))
    return plaintext == EXPECTED_VERIFIER_PLAINTEXT


def process_csv_file(filepath: Path, rel_path: Path, key: bytes):
    """Returns (output_bytes, rows_written, rows_dropped_bad_tag, rows_dropped_enc_fail)."""
    with open(filepath, "rb") as f:
        raw = f.read()
    lines = raw.splitlines()
    if len(lines) < 3:
        print(f"warning: {rel_path}: too short to have a valid header, skipping file", file=sys.stderr)
        return b"", 0, 0, 0

    header_line = lines[0]          # the AAD, verbatim -- never reconstructed
    encrypted = lines[1].strip() == ENCRYPTED_MARKER
    csv_header = lines[2]

    out_lines = [header_line, csv_header]
    rows_written = 0
    rows_bad_tag = 0
    rows_enc_fail = 0

    for i in range(3, len(lines)):
        line = lines[i].strip()
        if not line:
            continue
        line_num = i + 1   # 1-based, matches a text editor's line numbers

        if not encrypted:
            out_lines.append(line)
            rows_written += 1
            continue

        if line == ENC_FAIL_MARKER:
            rows_enc_fail += 1
            continue   # the firmware already told us this row never had plaintext to recover

        try:
            blob = bytes.fromhex(line.decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            print(f"warning: {rel_path}:{line_num}: not valid hex, skipping row", file=sys.stderr)
            rows_bad_tag += 1
            continue

        plaintext = gcm_open(key, blob, header_line)
        if plaintext is None:
            print(f"warning: {rel_path}:{line_num}: bad GCM tag, skipping row", file=sys.stderr)
            rows_bad_tag += 1
            continue

        out_lines.append(plaintext)
        rows_written += 1

    content = b"\n".join(out_lines) + b"\n"
    return content, rows_written, rows_bad_tag, rows_enc_fail


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Decrypt a pulled wifi_d_vice client folder and bundle it into one zip."
    )
    parser.add_argument("client_dir", help="Path to a pulled copy of /<client>/ from the SD card")
    parser.add_argument("--passphrase", required=True, help="The client's engagement passphrase")
    parser.add_argument("-o", "--output", default=None, help="Output zip path (default: <client>_export.zip)")
    args = parser.parse_args()

    client_dir = Path(args.client_dir)
    if not client_dir.is_dir():
        print(f"error: {client_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    eng = read_eng_txt(client_dir)
    salt = eng.get("salt")
    iters_str = eng.get("iters")
    verifier_hex = eng.get("verifier")
    if salt is None or iters_str is None or not verifier_hex:
        print("error: eng.txt missing required field(s): salt, iters, verifier", file=sys.stderr)
        sys.exit(1)
    try:
        iters = int(iters_str)
    except ValueError:
        print("error: invalid 'iters' value in eng.txt", file=sys.stderr)
        sys.exit(1)

    key = derive_key(args.passphrase, salt, iters)
    if not verify_passphrase(key, verifier_hex, salt):
        print("wrong passphrase", file=sys.stderr)
        sys.exit(1)

    out_path = Path(args.output) if args.output else Path(f"{client_dir.name}_export.zip")
    csv_files = sorted(client_dir.rglob("*.csv"))

    manifest_lines = []
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for csv_file in csv_files:
            rel_path = csv_file.relative_to(client_dir)
            content, written, bad_tag, enc_fail = process_csv_file(csv_file, rel_path, key)
            zf.writestr(str(rel_path), content)
            manifest_lines.append(
                f"{rel_path}: rows={written} dropped_bad_tag={bad_tag} dropped_enc_fail={enc_fail}"
            )
        zf.writestr("MANIFEST.txt", ("\n".join(manifest_lines) + "\n") if manifest_lines else "")

    print(f"exported {len(csv_files)} file(s) -> {out_path}")


if __name__ == "__main__":
    main()
