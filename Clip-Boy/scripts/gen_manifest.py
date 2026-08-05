#!/usr/bin/env python3
"""Generate release/flash-spec.json + release/SHA256SUMS over an OUT dir that
already holds the clipboy-*.bin set. Single-sourced so both make_release_bins.sh
(dev build) and release_from_staging.sh (build-from-staging) emit identical manifests.

Usage: gen_manifest.py <OUT_DIR> <VERSION> <variant> [<variant> ...]
  e.g. gen_manifest.py .../release 2026-07-18+abc1234 sn34k res34rch sn34k-rift res34rch-rift

Both files are written LF (newline='\n'): git normalizes the committed blob to LF,
and the minisign signature + web_flash's in-browser hash check are over the LF bytes
(DC34-122/123). flash-spec.json is hashed into SHA256SUMS, so it is written first.
"""
import json, sys, os, glob, hashlib

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    if len(sys.argv) < 4:
        print("usage: gen_manifest.py <OUT_DIR> <VERSION> <variant>...", file=sys.stderr)
        sys.exit(1)
    out, version, variants = sys.argv[1], sys.argv[2], sys.argv[3:]

    BOOT = [
        {"offset": 0x0,    "path": "clipboy-bootloader.bin", "role": "bootloader"},
        {"offset": 0x8000, "path": "clipboy-partitions.bin", "role": "partitions"},
        {"offset": 0xe000, "path": "clipboy-boot_app0.bin",  "role": "boot_app0"},
    ]
    LFS_PATH, LFS_OFFSET = "clipboy-littlefs.bin", 0x980000
    lfs_present = os.path.isfile(os.path.join(out, LFS_PATH))

    spec = {"chipFamily": "ESP32-S3", "version": version, "variants": {}}
    if lfs_present:
        spec["contentVersion"] = sha256(os.path.join(out, LFS_PATH))[:12]

    for v in variants:
        d = {"active": "res34rch" in v}
        if "rift" in v:
            d["rift"] = True
        parts = [dict(p) for p in BOOT] + [
            {"offset": 0x10000, "path": f"clipboy-{v}-app.bin", "role": "app"}]
        if lfs_present:
            parts.append({"offset": LFS_OFFSET, "path": LFS_PATH, "role": "littlefs"})
        for p in parts:
            p["sha256"] = sha256(os.path.join(out, p["path"]))
        d["parts"] = parts
        spec["variants"][v] = d

    with open(os.path.join(out, "flash-spec.json"), "w", newline="\n") as f:
        json.dump(spec, f, indent=2)
        f.write("\n")
    print(f"[manifest] wrote flash-spec.json ({len(variants)} variants, sha256 embedded)")

    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(out, "clipboy-*.bin")))
    names.append("flash-spec.json")
    with open(os.path.join(out, "SHA256SUMS"), "w", newline="\n") as f:
        for name in names:
            f.write(f"{sha256(os.path.join(out, name))}  {name}\n")
    print(f"[manifest] wrote SHA256SUMS ({len(names)} files)")

if __name__ == "__main__":
    main()
