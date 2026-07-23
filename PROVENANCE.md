# Authorship Provenance

This project is the original work of **Steven Jin**
(`stevenjin20090101@gmail.com`). This file explains how anyone can
**cryptographically verify** that Steven Jin authored these exact files — and
how Steven can prove it even if every visible credit is stripped out.

## The layers of proof

| Layer | Where | Strength |
|---|---|---|
| Copyright headers | top of every source file, `LICENSE`, `AUTHORS` | legal / attribution |
| Binary watermark | compiled into the firmware — `strings firmware.bin \| grep PPFW` | survives in a copied `.bin` |
| Invisible watermark | zero-width characters in `gui/piano-control.html` | survives copy/paste of the page |
| **Ed25519 signature** | `provenance/` | **cryptographic proof of authorship** |

## The cryptographic signature (the real proof)

Steven holds a secret **Ed25519 private key** (never in this repo). The matching
**public key** is committed at
[`provenance/author_ed25519_public.pem`](provenance/author_ed25519_public.pem),
public fingerprint **`eab16a502f679465`**.

`provenance/MANIFEST.txt` lists every source file with its SHA-256 hash, and
`provenance/MANIFEST.sig` is that manifest **signed with the private key**.
Because only Steven holds the private key, only Steven could have produced a
valid signature over these files — and the git commit date timestamps *when*.

### Verify it yourself

```bash
cd piano_firmware
python3 provenance/verify.py
```

The script recomputes every file's hash, checks them against the manifest, and
verifies the signature against the public key. It prints **`AUTHORSHIP VERIFIED`**
only if the files are unmodified and genuinely signed by Steven Jin's key.
(Needs the `cryptography` Python package: `pip install cryptography`.)

### Why this stops theft

- A thief who copies the files **cannot forge a new valid signature** for any
  modification — they don't have the private key.
- If someone strips the headers and claims the work, Steven signs a fresh
  challenge with his private key on demand — instantly proving he controls the
  key that signed the original, timestamped commit.
- Even a stripped binary still contains the `PPFW-PROVENANCE` watermark tying it
  back to this signature (fingerprint `eab16a502f679465`).

## For Steven — keep this safe

Your private key is at `~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem`.

- **Never commit it. Never share it.** It is `.gitignore`d.
- Back it up somewhere private (password manager / encrypted drive).
- If you ever need to re-prove authorship, sign any text with it:
  ```bash
  # sign a challenge
  python3 -c "from cryptography.hazmat.primitives.serialization import load_pem_private_key; \
  k=load_pem_private_key(open('$HOME/piano-authorship-PRIVATE-DO-NOT-SHARE.pem','rb').read(),None); \
  open('challenge.sig','wb').write(k.sign(b'I am Steven Jin, author of Player Piano'))"
  ```
  Anyone can verify that `challenge.sig` against the public key in this repo.

---

*Player Piano — © 2026 Steven Jin. Licensed under the MIT License.*
