#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
build_site.py — assemble the GitHub Pages web flasher from published releases.

ESP Web Tools fetches firmware from the browser, and GitHub release assets
do not send CORS headers, so the binaries are copied INTO the Pages site:

    _site/
      index.html, ...            (from web/)
      firmware/index.json        {releases: [{tag, boards: {env: {...}}}]}
      firmware/<tag>/<env>/      parts + manifest.json + board.json

Only published (non-draft) releases are included, newest first, capped by
--keep so the site stays small.
"""
import argparse
import io
import json
import os
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def http(url: str) -> bytes:
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "retimesh-build-site",
        **({"Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}"} if os.environ.get("GITHUB_TOKEN") else {}),
    })
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True, help="owner/name")
    ap.add_argument("--out", default="_site")
    ap.add_argument("--web", default=str(ROOT / "web"))
    ap.add_argument("--keep", type=int, default=5)
    args = ap.parse_args()

    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(args.web, out)
    (out / ".nojekyll").touch()
    fw_dir = out / "firmware"
    fw_dir.mkdir()

    releases = json.loads(http(f"https://api.github.com/repos/{args.repo}/releases?per_page=50"))
    releases = [r for r in releases if not r["draft"]]
    releases.sort(key=lambda r: r["published_at"], reverse=True)

    index = {"repo": args.repo, "releases": []}
    for rel in releases[: args.keep]:
        assets = {a["name"]: a for a in rel["assets"]}
        if "release.json" not in assets:
            print(f"skip {rel['tag_name']}: no release.json asset", file=sys.stderr)
            continue
        rj = json.loads(http(assets["release.json"]["browser_download_url"]))
        entry = {"tag": rel["tag_name"], "name": rel["name"], "published_at": rel["published_at"],
                 "prerelease": rel["prerelease"], "html_url": rel["html_url"], "boards": {}}
        for env, b in rj["boards"].items():
            if b["archive"] not in assets:
                print(f"skip {rel['tag_name']}/{env}: archive missing", file=sys.stderr)
                continue
            dest = fw_dir / rel["tag_name"] / env
            dest.mkdir(parents=True)
            with zipfile.ZipFile(io.BytesIO(http(assets[b["archive"]]["browser_download_url"]))) as z:
                for member in z.namelist():
                    if member.endswith((".bin", ".json")) and "merged" not in member:
                        z.extract(member, dest)
            entry["boards"][env] = {
                "name": b["name"], "notes": b.get("notes", ""), "chip_family": b["chip_family"],
                "manifest": f"firmware/{rel['tag_name']}/{env}/manifest.json",
                "archive_url": assets[b["archive"]]["browser_download_url"],
                "merged_url": assets[b["merged"]["file"]]["browser_download_url"]
                              if b["merged"]["file"] in assets else None,
                "merged_sha256": b["merged"]["sha256"],
            }
        index["releases"].append(entry)

    (fw_dir / "index.json").write_text(json.dumps(index, indent=2) + "\n")
    print(f"site: {len(index['releases'])} release(s) -> {out}")


if __name__ == "__main__":
    main()
