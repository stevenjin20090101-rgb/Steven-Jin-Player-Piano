# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
# tools/fw_build_id.py — PlatformIO pre-script: <build dir>/generated/fw_build.h
#
# FW_BUILD = the 7-character git hash, "-dirty" appended when the tree has changes
# (untracked files included), "nogit" without git (docs/BLE_OTA.md section 2). A
# generated header rather than a -D flag: a flag would change every file's and
# library's flags per commit and rebuild LVGL. Only src/fw_info.cpp includes it.
Import("env")
import os, subprocess
def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env.subst("$PROJECT_DIR"),
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""
build = git("rev-parse", "--short=7", "HEAD") or "nogit"
if build != "nogit" and git("status", "--porcelain"):
    build += "-dirty"
gen = os.path.join(env.subst("$BUILD_DIR"), "generated")
os.makedirs(gen, exist_ok=True)
path, text = os.path.join(gen, "fw_build.h"), '#pragma once\n#define FW_BUILD "%s"\n' % build
if not os.path.exists(path) or open(path).read() != text:   # rewrite only on change
    open(path, "w").write(text)
env.Append(CPPPATH=[gen])
