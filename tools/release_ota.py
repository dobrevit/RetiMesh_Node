#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.
"""Build, sign and bundle an over-the-air update, per board.

Three steps have to agree with each other for a node to accept an update, and
each of them is a place to get it wrong by hand:

  the image     `pio run -e <env>`, with the core-3 package isolation every
                invocation in this checkout needs
  the manifest  `fw_sign.py sign`, which has to be told the board, the version,
                and the size of the slot the image will be written into
  the bundle    `fw_sign.py bundle`, written as `.rmfw` because that is what
                the portal's upload control will let an operator pick

The three arguments worth automating are exactly the three that are silent when
wrong:

  --board is the **PlatformIO env name**, because that is what the firmware
    compares against (FirmwareManifest.h: "this build's env"). A manifest built
    for `t3s3` and offered to a T-Deck is refused as "built for another board",
    which reads like a corrupt download.

  --slot-size is **this board's app partition**, read here from the same CSV
    the bootloader obeys rather than typed. Typing it is how a t3s3's 1966080
    ends up on a 16 MB board: the node then refuses an image that would have
    fitted, or — worse in the other direction — accepts a manifest whose slot
    figure does not describe the partition it is about to write.

  --version is what `git describe` says, matching what the running firmware
    reports, so a node comparing the two is comparing like with like.

It also refuses outright to build an OTA for a board that cannot install one.
Seven of the twelve envs here have a single app partition; a node on one of
those has nothing to write an update into (OtaInstaller.h: NoSlot) and must be
flashed by cable. Producing a bundle for it anyway wastes a download and an
operator's afternoon.

    tools/release_ota.py t-deck
    tools/release_ota.py t-deck thinknode-m9 --secure-version 1
    tools/release_ota.py --all
    tools/release_ota.py t-deck --no-build      # sign what is already built

Keys default to the layout `fw_sign.py genkey` produces; --key takes the stem,
not the file, because the tool reads both `.key` and `.pub` beside each other.
"""

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW_SIGN = ROOT / "tools" / "fw_sign.py"

# Every pio call in this checkout carries these: the default ~/.platformio tree
# belongs to the RNode_Firmware mirror and the two platforms uninstall each
# other. Set here so an operator cannot forget them — the failure is a build
# against the wrong core, which surfaces as a missing header rather than as
# anything that names the real cause.
PIO_ENV = {
    "PLATFORMIO_PACKAGES_DIR": os.path.expanduser("~/.platformio/packages-core3"),
    "PLATFORMIO_PLATFORMS_DIR": os.path.expanduser("~/.platformio/platforms-core3"),
}


def die(msg):
    sys.exit(f"release_ota: {msg}")


def pio_bin():
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return found
    fallback = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if fallback.is_file():
        return str(fallback)
    die("cannot find `pio` on PATH or at ~/.platformio/penv/bin/pio")


def run(cmd, **kw):
    """Run a command, gated on its real exit code."""
    env = {**os.environ, **PIO_ENV, **kw.pop("extra_env", {})}
    proc = subprocess.run(cmd, cwd=ROOT, env=env, **kw)
    if proc.returncode != 0:
        die(f"{cmd[0]} exited {proc.returncode}: {' '.join(str(c) for c in cmd)}")
    return proc


def build(env_name: str):
    """`pio run -e <env>`, streaming, gated on the real exit code.

    With one retry, for one specific failure. PlatformIO's SCons database goes
    missing often enough in this checkout to have cost four reruns in an
    afternoon:

        *** [.../SPI.cpp] .pio/build/<env>/.sconsign314.dblite: No such file
            or directory

    That is a broken build directory, not a broken source tree, and the fix is
    always the same — delete it and build again. Retrying it here rather than
    leaving it to the operator matters because this script builds several
    boards in a row and dies on the first failure: without this, one corrupt
    directory costs every board after it too. Anything else fails as it should,
    once, with its own output on screen.
    """
    for attempt in (1, 2):
        proc = subprocess.Popen([pio_bin(), "run", "-e", env_name], cwd=ROOT,
                                env={**os.environ, **PIO_ENV},
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
        captured = []
        for line in proc.stdout:
            sys.stdout.write(line)
            captured.append(line)
        code = proc.wait()
        if code == 0:
            return
        stale = any(".sconsign" in l and "No such file" in l for l in captured)
        if stale and attempt == 1:
            build_dir = ROOT / ".pio" / "build" / env_name
            print(f"\n  ! {env_name}: PlatformIO's build database is missing. That is a "
                  f"broken build directory, not the code.\n"
                  f"    removing {build_dir} and building once more.\n")
            shutil.rmtree(build_dir, ignore_errors=True)
            continue
        die(f"`pio run -e {env_name}` exited {code}")


def project_config():
    out = subprocess.run([pio_bin(), "project", "config", "--json-output"],
                         cwd=ROOT, env={**os.environ, **PIO_ENV},
                         capture_output=True, text=True)
    if out.returncode != 0:
        die(f"`pio project config` exited {out.returncode}:\n{out.stderr.strip()}")
    return {s[4:]: dict(o) for s, o in json.loads(out.stdout) if s.startswith("env:")}


def resolve_table(name: str):
    """Where a `board_build.partitions` value actually is.

    A value with a path separator is ours, under partitions/. A bare filename
    is one of the framework's own tables (huge_app.csv and friends), which live
    in the core-3 package tree — not in this repository, which is what made the
    first version of this script crash rather than explain itself.
    """
    local = ROOT / name
    if local.is_file():
        return local
    fw = (Path(PIO_ENV["PLATFORMIO_PACKAGES_DIR"]) / "framework-arduinoespressif32"
          / "tools" / "partitions" / name)
    if fw.is_file():
        return fw
    return None


def app_slots(table: Path):
    """Every app partition in a table, smallest first.

    The same reading check_image_size.py does, and for the same reason: the
    table is what the bootloader obeys, so the figure has to come from it
    rather than from a number written down somewhere.
    """
    sizes = []
    with open(table, newline="") as f:
        for row in csv.reader(f):
            cells = [c.strip() for c in row if c.strip()]
            if len(cells) < 5 or cells[0].startswith("#"):
                continue
            if cells[1] != "app":
                continue
            raw = cells[4].lower()
            try:
                if raw.endswith("k"):
                    sizes.append(int(raw[:-1], 0) * 1024)
                elif raw.endswith("m"):
                    sizes.append(int(raw[:-1], 0) * 1024 * 1024)
                else:
                    sizes.append(int(raw, 0))
            except ValueError:
                continue
    return sorted(sizes)


def git_version():
    out = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                         cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        die("git describe failed; pass --version explicitly")
    v = out.stdout.strip()
    if v.endswith("-dirty"):
        print(f"  ! {v}: the working tree is dirty, so this image is not any commit",
              file=sys.stderr)
    if len(v) > 32:
        die(f"version '{v}' is {len(v)} chars; the manifest field holds 32")
    return v


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("envs", nargs="*", help="PlatformIO env names, e.g. t-deck")
    p.add_argument("--all", action="store_true",
                   help="every env whose partition table has two app slots")
    p.add_argument("--version", help="default: git describe")
    p.add_argument("--key", default=os.path.expanduser("~/.retimesh/keys/retimesh-signing-2026"),
                   help="signing key STEM; .key and .pub are read beside it")
    p.add_argument("--delegation", default=str(ROOT / "delegation.bin"))
    p.add_argument("--secure-version", type=int, default=1,
                   help="anti-rollback floor this image claims")
    p.add_argument("--out-dir", default=str(ROOT / "dist"))
    # Convenient, but do not rely on it: PlatformIO wipes .pio/build wholesale
    # when its project checksum moves, so an image built ten minutes ago may
    # simply not be there. The bundle in --out-dir is the durable artefact.
    p.add_argument("--no-build", action="store_true",
                   help="sign the image already in .pio/build/<env>, if it is still there")
    args = p.parse_args()

    cfg = project_config()

    def ota_slots(env_opts):
        """The app slots for an env, or [] where the table cannot be read."""
        name = env_opts.get("board_build.partitions")
        if not name:
            return []
        table = resolve_table(name)
        return app_slots(table) if table else []

    if args.all:
        envs = [e for e, o in cfg.items() if len(ota_slots(o)) >= 2]
    else:
        envs = args.envs
    if not envs:
        die("name at least one env, or pass --all")

    for e in envs:
        if e not in cfg:
            die(f"unknown env '{e}'; platformio.ini has: {', '.join(sorted(cfg))}")

    for stem, what in ((f"{args.key}.key", "private key"), (f"{args.key}.pub", "public key")):
        if not Path(stem).is_file():
            die(f"no {what} at {stem}")
    if not Path(args.delegation).is_file():
        die(f"no delegation at {args.delegation} — make one with `fw_sign.py delegate`")

    version = args.version or git_version()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    made = []
    for env in envs:
        name = cfg[env].get("board_build.partitions")
        if not name:
            die(f"{env} uses the framework's stock partition table and cannot be "
                f"sized from a CSV; it is not an OTA target")
        table = resolve_table(name)
        if not table:
            die(f"{env}: cannot find the partition table '{name}' — not under "
                f"{ROOT}/ and not in the core-3 framework package")
        slots = app_slots(table)
        if len(slots) < 2:
            # The check worth having. A single-app-partition board has nowhere
            # to write an update; the node refuses with NoSlot after downloading
            # the whole thing.
            die(f"{env} has {len(slots)} app partition(s) in {table} — this board "
                f"cannot install an OTA update at all and must be flashed by "
                f"cable. Skipping it is not a workaround; it has no second slot.")
        slot = slots[0]

        print(f"\n=== {env}  (slot {slot} bytes from {name})")
        if not args.no_build:
            build(env)

        image = ROOT / ".pio" / "build" / env / "firmware.bin"
        if not image.is_file():
            die(f"no image at {image}" + (" — drop --no-build" if args.no_build else ""))
        size = image.stat().st_size
        if size > slot:
            die(f"{env}: image is {size} bytes, larger than the {slot}-byte slot")

        manifest = out_dir / f"{env}-{version}.manifest.bin"
        # .rmfw, not .ota. The portal's file picker filters on it
        # (data/settings.html: accept=".rmfw"), the node stages it under that
        # name (OtaUpdate.h: STAGING_PATH), and fw_sign.py's own usage line
        # writes update.rmfw. A bundle with any other suffix is correct in
        # every byte and still cannot be selected in the browser.
        bundle = out_dir / f"{env}-{version}.rmfw"
        run([sys.executable, str(FW_SIGN), "sign",
             "--image", str(image), "--board", env, "--version", version,
             "--key", args.key, "--delegation", args.delegation,
             "--secure-version", str(args.secure_version),
             "--slot-size", str(slot), "--out", str(manifest)])
        run([sys.executable, str(FW_SIGN), "bundle",
             "--manifest", str(manifest), "--image", str(image), "--out", str(bundle)])
        made.append((env, bundle, size, slot))

    print("\n=== ready")
    for env, bundle, size, slot in made:
        pct = 100.0 * size / slot
        print(f"  {env:14} {bundle.name}  ({size} B, {pct:.1f}% of slot)")


if __name__ == "__main__":
    main()
