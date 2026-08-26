#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
make_manifest.py — turn PlatformIO build output into flashable release bundles.

  build --env t3s3 --version v1.0.0 [--out dist]
      Collects bootloader/partitions/boot_app0/firmware/littlefs from
      .pio/build/<env>, derives every flash offset from the env's partition
      table (never hard-coded), and writes to dist/<env>/:
        * the raw parts                         (for app-only / fs-only flashing)
        * retimesh-node-<ver>-<env>-merged.bin   (single image, flash at 0x0)
        * manifest.json                          (ESP Web Tools format)
        * board.json                             (machine-readable description)
        * retimesh-node-<ver>-<env>.zip          (everything above)

  index --dist dist --version v1.0.0 --repo owner/name
      Merges all dist/*/board.json into dist/release.json and writes
      dist/sha256sums.txt — the discovery documents used by the web flasher
      and the retimesh-flash CLI.
"""
import argparse
import configparser
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
FRAMEWORK = CORE / "packages" / "framework-arduinoespressif32"
BOARDS = json.loads((ROOT / "boards.json").read_text())

# Second-stage bootloader offset differs per chip family.
BOOTLOADER_OFFSET = {"esp32": 0x1000}          # everything else: 0x0
PARTITION_TABLE_OFFSET = 0x8000
BOOT_APP0_OFFSET = 0xE000


def esptool_opt(name: str) -> str:
    """esptool >= 5 uses dashes (merge-bin, --flash-mode); 4.x underscores."""
    try:
        import esptool
        major = int(esptool.__version__.split(".")[0])
    except Exception:
        major = 4
    return name.replace("_", "-") if major >= 5 else name


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def env_config(env: str) -> dict:
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(ROOT / "platformio.ini")
    section = f"env:{env}"
    if section not in cp:
        sys.exit(f"platformio.ini has no [{section}]")

    def get(key, default=None):
        return cp[section].get(key) or cp["env"].get(key) or default

    return {
        "board": get("board"),
        "partitions": get("board_build.partitions", "default.csv"),
        "flash_size": get("board_upload.flash_size"),
    }


def board_json(board: str) -> dict:
    for candidate in (ROOT / "boards" / f"{board}.json",
                      CORE / "platforms" / "espressif32" / "boards" / f"{board}.json"):
        if candidate.exists():
            return json.loads(candidate.read_text())
    sys.exit(f"board definition {board}.json not found")


def partition_offsets(csv_name: str) -> tuple[int, int]:
    for candidate in (ROOT / csv_name, FRAMEWORK / "tools" / "partitions" / csv_name):
        if candidate.exists():
            csv = candidate
            break
    else:
        sys.exit(f"partition table {csv_name} not found")

    app_off = fs_off = None
    for line in csv.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, ptype, subtype, offset, *_ = [c.strip() for c in line.split(",")]
        if ptype == "app" and app_off is None:
            app_off = int(offset, 0)
        if subtype in ("spiffs", "littlefs") and fs_off is None:
            fs_off = int(offset, 0)
    if app_off is None or fs_off is None:
        sys.exit(f"{csv}: could not find app / filesystem partitions")
    return app_off, fs_off


def cmd_build(args):
    env, version = args.env, args.version
    if env not in BOARDS:
        sys.exit(f"{env} is not listed in boards.json")
    meta = BOARDS[env]
    cfg = env_config(env)
    bj = board_json(cfg["board"])
    mcu = bj["build"]["mcu"]
    flash_mode = bj["build"].get("flash_mode", "dio")
    flash_size = cfg["flash_size"] or bj["upload"]["flash_size"]
    flash_freq = str(int(bj["build"].get("f_flash", "40000000L").rstrip("L")) // 1_000_000) + "m"
    app_off, fs_off = partition_offsets(cfg["partitions"])

    build_dir = ROOT / ".pio" / "build" / env
    parts = [
        (BOOTLOADER_OFFSET.get(mcu, 0x0), build_dir / "bootloader.bin"),
        (PARTITION_TABLE_OFFSET,           build_dir / "partitions.bin"),
        (BOOT_APP0_OFFSET,                 FRAMEWORK / "tools" / "partitions" / "boot_app0.bin"),
        (app_off,                          build_dir / "firmware.bin"),
        (fs_off,                           build_dir / "littlefs.bin"),
    ]
    for _, p in parts:
        if not p.exists():
            sys.exit(f"missing {p} — run `pio run -e {env}` and `pio run -e {env} -t buildfs` first")

    out = Path(args.out) / env
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    part_entries = []
    for off, src in parts:
        dst = out / src.name
        shutil.copy2(src, dst)
        part_entries.append({"file": src.name, "offset": off, "size": dst.stat().st_size,
                             "sha256": sha256(dst)})

    merged_name = f"retimesh-node-{version}-{env}-merged.bin"
    merged = out / merged_name
    cmd = [sys.executable, "-m", "esptool", "--chip", mcu, esptool_opt("merge_bin"), "-o", str(merged),
           esptool_opt("--flash_mode"), flash_mode, esptool_opt("--flash_freq"), flash_freq,
           esptool_opt("--flash_size"), flash_size]
    for e in part_entries:
        cmd += [hex(e["offset"]), str(out / e["file"])]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    manifest = {
        "name": f"RetiMesh Node — {meta['name']}",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": meta["chip_family"],
            "parts": [{"path": e["file"], "offset": e["offset"]} for e in part_entries],
        }],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    archive_name = f"retimesh-node-{version}-{env}.zip"
    board = {
        "env": env, "name": meta["name"], "notes": meta.get("notes", ""),
        "chip": mcu, "chip_family": meta["chip_family"],
        "flash_mode": flash_mode, "flash_size": flash_size, "flash_freq": flash_freq,
        "app_offset": app_off, "fs_offset": fs_off,
        "parts": part_entries,
        "merged": {"file": merged_name, "offset": 0, "size": merged.stat().st_size,
                   "sha256": sha256(merged)},
        "archive": archive_name,
    }
    (out / "board.json").write_text(json.dumps(board, indent=2) + "\n")

    with zipfile.ZipFile(out / archive_name, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(out.iterdir()):
            if f.name != archive_name:
                z.write(f, f.name)

    print(f"{env}: app@{hex(app_off)} fs@{hex(fs_off)} merged={merged.stat().st_size} B -> {out}")


def cmd_index(args):
    dist = Path(args.dist)
    boards = {}
    for bj in sorted(dist.glob("*/board.json")):
        b = json.loads(bj.read_text())
        boards[b["env"]] = b
    if not boards:
        sys.exit(f"no */board.json under {dist}")

    base = f"https://github.com/{args.repo}/releases/download/{args.version}/"
    for b in boards.values():
        b["archive_url"] = base + b["archive"]
        b["merged"]["url"] = base + b["merged"]["file"]

    release = {"firmware": "RetiMesh Node", "version": args.version, "repo": args.repo,
               "boards": boards}
    (dist / "release.json").write_text(json.dumps(release, indent=2) + "\n")

    lines = []
    for f in sorted(dist.rglob("*")):
        if f.is_file() and f.name != "sha256sums.txt":
            lines.append(f"{sha256(f)}  {f.relative_to(dist)}")
    (dist / "sha256sums.txt").write_text("\n".join(lines) + "\n")
    print(f"release.json: {', '.join(boards)}; {len(lines)} files hashed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("--env", required=True); b.add_argument("--version", required=True)
    b.add_argument("--out", default="dist"); b.set_defaults(func=cmd_build)
    i = sub.add_parser("index"); i.add_argument("--dist", default="dist"); i.add_argument("--version", required=True)
    i.add_argument("--repo", required=True); i.set_defaults(func=cmd_index)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
