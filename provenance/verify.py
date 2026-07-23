#!/usr/bin/env python3
"""Verify the authorship of this repository.

Recomputes the SHA-256 of every file listed in MANIFEST.txt, checks them, then
verifies MANIFEST.sig against the committed Ed25519 public key. Prints
"AUTHORSHIP VERIFIED" only if the files are unmodified and were signed by the
holder of the private key (Steven Jin).

Usage:  python3 provenance/verify.py
Needs:  pip install cryptography
"""
import os, sys, hashlib, base64

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "MANIFEST.txt")
SIG = os.path.join(HERE, "MANIFEST.sig")
PUB = os.path.join(HERE, "author_ed25519_public.pem")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    try:
        from cryptography.hazmat.primitives.serialization import load_pem_public_key
        from cryptography.exceptions import InvalidSignature
    except ImportError:
        print("ERROR: pip install cryptography"); return 2

    with open(MANIFEST, "r", encoding="utf-8") as f:
        manifest_text = f.read()

    # 1) check every file hash
    bad = 0
    for line in manifest_text.splitlines():
        if not line or line.startswith("#") or "  " not in line:
            continue
        digest, rel = line.split("  ", 1)
        path = os.path.join(REPO, rel)
        if not os.path.exists(path):
            print("MISSING:", rel); bad += 1; continue
        if sha256(path) != digest:
            print("MODIFIED:", rel); bad += 1

    # 2) verify the signature over the manifest
    pub = load_pem_public_key(open(PUB, "rb").read())
    sig = base64.b64decode(open(SIG, "rb").read())
    sig_ok = True
    try:
        pub.verify(sig, manifest_text.encode("utf-8"))
    except InvalidSignature:
        sig_ok = False

    print("-" * 60)
    if bad == 0 and sig_ok:
        print("AUTHORSHIP VERIFIED — files unmodified and signed by")
        print("Steven Jin (Ed25519 fp eab16a502f679465).")
        return 0
    if not sig_ok:
        print("SIGNATURE INVALID — this is not Steven Jin's signed tree.")
    if bad:
        print(f"{bad} file(s) do not match the signed manifest.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
