#!/usr/bin/env bash
# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
#
# STATUS: UNTESTED against the real repository. Written 2026-09-27, before include/config.h
# carried FW_VERSION (it refuses to run until it does). Run it with --dry-run first; its first
# real run is the 2.0.0 release. Delete this paragraph once that release has gone through.
#
# Publishes a piano firmware release, so the Steven Piano app can offer it over Bluetooth
# (docs/BLE_OTA.md section 10):
#
#   tools/publish-firmware.sh "<notes>" [--usb-only] [--dry-run]
#   PIANO_MIN_APP_VERSION_CODE=<versionCode> tools/publish-firmware.sh "Softer pianissimo." --dry-run
#
# Before running it: set FW_VERSION in include/config.h, update the docs, commit, re-sign
# provenance (~/.platformio/penv/bin/python provenance/sign.py && ... provenance/verify.py),
# commit, push. The tree must be clean and on main. It needs PlatformIO in ~/.platformio/penv
# (whose python has `cryptography`), the GitHub CLI signed in (`gh auth login`), and:
#   PIANO_SIGNING_KEY           the Ed25519 private key, default
#                               ~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem. Never in the repo;
#                               this script never prints it.
#   PIANO_MIN_APP_VERSION_CODE  the lowest Steven Piano versionCode that may install this
#                               firmware (required; the manifest's minAppVersionCode).
#
# What it does, in order, stopping at the first problem:
#   1. checks: a clean tree on main that is not behind GitHub; FW_VERSION; no tag or release
#      fw-v<version> yet; provenance verifies; the signing key is the one in
#      provenance/author_ed25519_public.pem, and include/ota_pubkey.h embeds the same key;
#   2. builds (pio run) and checks the image: an ESP32-S3 application, no larger than the OTA
#      slot or the app's 4 MB cap, carrying the version string "<version>+<commit>";
#   3. copies it to firmware-<version>.bin, takes its SHA-256, signs the 32 digest bytes and
#      verifies that signature with the repository's public key;
#   4. pushes main, creates the GitHub release fw-v<version> at the built commit with the binary
#      attached, then downloads it back through the manifest's URL and compares the SHA-256;
#   5. writes releases/latest.json, re-signs provenance, commits both and pushes.
# The release exists before the manifest that names it is pushed, so the app never reads a
# manifest whose binary is missing. --dry-run stops after step 3 and prints the manifest.
# --usb-only marks a release that changes the partition table or the bootloader: the app lists
# it but says it needs a USB flash, and never sends it over Bluetooth.

set -euo pipefail

# The app allow-lists this exact name (docs/BLE_OTA.md section 15). GitHub renamed the repository
# on 2026-09-27; if it changes again, change the app's allow-list in the same release.
REPO="stevenjin20090101-rgb/Steven-Jin-Player-Piano"
PIO_ENV="lilygo-t4-s3"
SLOT_BYTES=6553600                    # app0 / app1 in default_16MB.csv: 0x640000
APP_CAP_BYTES=$((4 * 1024 * 1024))    # the app refuses a larger firmware download
CO_AUTHOR="Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"

die() {
    echo "publish-firmware: $*" >&2
    exit 1
}
usage() {
    echo "usage: tools/publish-firmware.sh \"<notes>\" [--usb-only] [--dry-run]" >&2
    exit 64
}

NOTES=""
USB_ONLY=false
DRY_RUN=false
for arg in "$@"; do
    case "$arg" in
        --usb-only) USB_ONLY=true ;;
        --dry-run)  DRY_RUN=true ;;
        -*)         usage ;;
        *)          [ -z "$NOTES" ] || usage; NOTES="$arg" ;;
    esac
done
[ -n "$NOTES" ] || usage
[ "${#NOTES}" -le 1000 ] || die "The notes are ${#NOTES} characters; keep them to 1,000."

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PY="$HOME/.platformio/penv/bin/python"
PIO="$HOME/.platformio/penv/bin/pio"
PROVENANCE_KEY="$HOME/piano-authorship-PRIVATE-DO-NOT-SHARE.pem"   # provenance/sign.py's fixed path
export PIANO_SIGNING_KEY="${PIANO_SIGNING_KEY:-$PROVENANCE_KEY}"   # read by the Python helpers only
MIN_APP="${PIANO_MIN_APP_VERSION_CODE:-}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# --- Python helpers (PlatformIO's python has `cryptography`; the system python3 may not) -------
cat >"$STAGE/keycheck.py" <<'PY'
# The signing key must be the author key this repository publishes, and the firmware must embed it.
import os, re, sys
from cryptography.hazmat.primitives.serialization import (
    Encoding, PublicFormat, load_pem_private_key, load_pem_public_key)
def raw(key):
    return key.public_bytes(Encoding.Raw, PublicFormat.Raw)
try:
    with open(os.environ["PIANO_SIGNING_KEY"], "rb") as f:
        signer = raw(load_pem_private_key(f.read(), password=None).public_key())
except Exception:
    sys.exit("the signing key (PIANO_SIGNING_KEY) could not be loaded")
with open("provenance/author_ed25519_public.pem", "rb") as f:
    published = raw(load_pem_public_key(f.read()))
if signer != published:
    sys.exit("the signing key is not the one in provenance/author_ed25519_public.pem")
try:
    with open("include/ota_pubkey.h") as f:
        text = f.read()
except OSError:
    sys.exit("include/ota_pubkey.h is missing (docs/BLE_OTA.md section 8)")
body = re.search(r"kOtaPublicKey\s*\[\s*32\s*\]\s*=\s*\{([^}]*)\}", text)
embedded = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body.group(1))) if body else b""
if embedded != published:
    sys.exit("include/ota_pubkey.h does not hold the key in provenance/author_ed25519_public.pem")
PY

cat >"$STAGE/imagecheck.py" <<'PY'
import struct, sys
path, full, slot, cap = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
with open(path, "rb") as f:
    data = f.read()
if len(data) < 24 or data[0] != 0xE9 or struct.unpack_from("<H", data, 12)[0] != 0x0009:
    sys.exit("the build is not an ESP32-S3 application image")
if len(data) > min(slot, cap):
    sys.exit("the image is %d bytes: more than the OTA slot (%d) or the app's cap (%d)" % (len(data), slot, cap))
if (full + "\0").encode() not in data:
    sys.exit("the image does not carry the version string %s (a -dirty or stale build?)" % full)
print(len(data))
PY

cat >"$STAGE/sign.py" <<'PY'
# Prints "<sha256 hex> <base64 signature>". The signature is Ed25519 over the 32 raw digest bytes.
import base64, hashlib, os, sys
from cryptography.hazmat.primitives.serialization import load_pem_private_key, load_pem_public_key
with open(sys.argv[1], "rb") as f:
    digest = hashlib.sha256(f.read()).digest()
try:
    with open(os.environ["PIANO_SIGNING_KEY"], "rb") as f:
        sig = load_pem_private_key(f.read(), password=None).sign(digest)
except Exception:
    sys.exit("signing failed: the key at PIANO_SIGNING_KEY could not be used")
with open("provenance/author_ed25519_public.pem", "rb") as f:
    load_pem_public_key(f.read()).verify(sig, digest)   # raises unless it is the author key
print(digest.hex(), base64.b64encode(sig).decode())
PY

cat >"$STAGE/manifest.py" <<'PY'
import json, sys
version, build, url, size, sha, sig, min_app, notes, usb_only, out = sys.argv[1:]
manifest = {
    "version": version, "build": build, "binUrl": url, "sizeBytes": int(size),
    "sha256": sha, "sig": sig, "minAppVersionCode": int(min_app), "notes": notes,
    "usbOnly": usb_only == "true",
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY

# --- 1. Checks -------------------------------------------------------------------------------
[ -x "$PY" ] || die "PlatformIO's python is missing at ~/.platformio/penv/bin/python."
[ -x "$PIO" ] || die "PlatformIO is missing at ~/.platformio/penv/bin/pio."
"$PY" -c "import cryptography" 2>/dev/null \
    || die "PlatformIO's python lacks cryptography: ~/.platformio/penv/bin/python -m pip install cryptography"
command -v gh >/dev/null || die "The GitHub CLI (gh) is not installed: brew install gh"
gh auth status >/dev/null 2>&1 || die "gh is not signed in: gh auth login"
# gh talks to the repository under the name GitHub resolves today; the manifest keeps REPO's name.
GH_REPO="$(gh repo view "$REPO" --json nameWithOwner --jq .nameWithOwner)" || die "gh cannot see $REPO."
command -v curl >/dev/null || die "curl is needed to check the uploaded binary."
[ -r "$PIANO_SIGNING_KEY" ] \
    || die "The signing key is not readable (PIANO_SIGNING_KEY, default ~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem)."
[ -r "$PROVENANCE_KEY" ] \
    || die "provenance/sign.py signs with ~/piano-authorship-PRIVATE-DO-NOT-SHARE.pem, which is missing."
[[ "$MIN_APP" =~ ^[0-9]+$ ]] \
    || die "Set PIANO_MIN_APP_VERSION_CODE to the lowest Steven Piano versionCode that may install this firmware."

[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || die "Not on main."
[ -z "$(git status --porcelain)" ] || die "The tree has uncommitted changes; commit them (and re-sign provenance) first."
git fetch --quiet origin main
git merge-base --is-ancestor origin/main HEAD || die "main is behind GitHub's; pull first."

VERSION_LINES="$(grep -cE '^#define[[:space:]]+FW_VERSION[[:space:]]' include/config.h || true)"
[ "$VERSION_LINES" = "1" ] \
    || die "include/config.h must define FW_VERSION once (#define FW_VERSION \"X.Y.Z\"); this script needs firmware 2.0.0 or later."
VERSION="$(sed -nE 's/^#define[[:space:]]+FW_VERSION[[:space:]]+"([0-9]+\.[0-9]+\.[0-9]+)".*/\1/p' include/config.h)"
[ -n "$VERSION" ] || die "FW_VERSION in include/config.h is not \"MAJOR.MINOR.PATCH\"."
[ "${#VERSION}" -le 16 ] || die "FW_VERSION is longer than the 16 bytes the BEGIN frame carries."

TAG="fw-v$VERSION"
ASSET="firmware-$VERSION.bin"
BIN_URL="https://github.com/$REPO/releases/download/$TAG/$ASSET"
SOURCE="$(git rev-parse HEAD)"
BUILD="$(git rev-parse --short=7 HEAD)"

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || [ -n "$(git ls-remote --tags origin "refs/tags/$TAG")" ]; then
    die "The tag $TAG exists already: raise FW_VERSION."
fi
if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
    die "The GitHub release $TAG exists already."
fi

# Output is captured before it is searched: with pipefail, grep -q closing the pipe early could fail the check.
PROVENANCE_OUT="$("$PY" provenance/verify.py 2>&1)" || die "provenance/verify.py does not verify: re-sign provenance and commit it first."
grep -q "AUTHORSHIP VERIFIED" <<<"$PROVENANCE_OUT" || die "provenance/verify.py does not verify: re-sign provenance and commit it first."
"$PY" "$STAGE/keycheck.py" || die "The key check failed (see above)."

# --- 2. Build and check ----------------------------------------------------------------------
"$PIO" run -e "$PIO_ENV"
BIN=".pio/build/$PIO_ENV/firmware.bin"
[ -s "$BIN" ] || die "The build left no $BIN."
[ -z "$(git status --porcelain)" ] || die "The build changed files in the tree; see git status."
SIZE="$("$PY" "$STAGE/imagecheck.py" "$BIN" "$VERSION+$BUILD" "$SLOT_BYTES" "$APP_CAP_BYTES")" \
    || die "The image check failed (see above)."

# --- 3. Copy, hash, sign ---------------------------------------------------------------------
# gh's "file#label" only sets a display label; the download URL uses the file's own name.
cp "$BIN" "$STAGE/$ASSET"
SIGNED="$("$PY" "$STAGE/sign.py" "$STAGE/$ASSET")" || die "Signing failed (see above)."
SHA256="${SIGNED%% *}"
SIG="${SIGNED##* }"
[ "${#SHA256}" -eq 64 ] && [ "${#SIG}" -eq 88 ] || die "Unexpected output from the signing step."
echo "Built $ASSET: $SIZE bytes, version $VERSION+$BUILD, SHA-256 $SHA256"

if $DRY_RUN; then
    "$PY" "$STAGE/manifest.py" "$VERSION" "$BUILD" "$BIN_URL" "$SIZE" "$SHA256" "$SIG" "$MIN_APP" "$NOTES" "$USB_ONLY" "$STAGE/latest.json"
    echo "Dry run: nothing pushed, released or committed. A real run creates $TAG at $SOURCE with $ASSET and publishes:"
    cat "$STAGE/latest.json"
    exit 0
fi

# --- 4. The release, at the commit that built it --------------------------------------------
git push origin HEAD:main
gh release create "$TAG" "$STAGE/$ASSET#$ASSET" --repo "$GH_REPO" --target "$SOURCE" \
    --title "Piano firmware $VERSION" --notes "$NOTES"
curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors -o "$STAGE/check.bin" "$BIN_URL" \
    || die "The release $TAG exists, but $BIN_URL does not download; the manifest was not published."
[ "$(shasum -a 256 "$STAGE/check.bin" | cut -d' ' -f1)" = "$SHA256" ] \
    || die "The release $TAG exists, but $BIN_URL is not the file built; the manifest was not published."

# --- 5. The manifest the app reads -----------------------------------------------------------
mkdir -p releases
"$PY" "$STAGE/manifest.py" "$VERSION" "$BUILD" "$BIN_URL" "$SIZE" "$SHA256" "$SIG" "$MIN_APP" "$NOTES" "$USB_ONLY" releases/latest.json
"$PY" provenance/sign.py
PROVENANCE_OUT="$("$PY" provenance/verify.py 2>&1)" || die "provenance/verify.py does not verify after re-signing."
grep -q "AUTHORSHIP VERIFIED" <<<"$PROVENANCE_OUT" || die "provenance/verify.py does not verify after re-signing."
git add releases/latest.json provenance/MANIFEST.txt provenance/MANIFEST.sig
git commit --quiet -F - <<EOF
Firmware $VERSION: releases/latest.json offers $TAG

$NOTES

$CO_AUTHOR
EOF
if ! git push origin main; then
    die "The release $TAG exists, but the manifest commit did not reach GitHub: run \`git push origin main\`."
fi
git fetch --quiet --tags origin

echo "Published piano firmware $VERSION ($TAG): $BIN_URL"
echo "The app offers it when its Firmware page next checks (on opening, or once a day)."
