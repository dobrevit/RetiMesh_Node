# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
"""Sign a firmware image, and manage the keys that are allowed to.

The trust chain is two offline roots -> a delegation naming a signing key -> a
manifest describing one image; src/sys/FirmwareManifest.h is the other half of
this file and describes the format and the checks a node makes. Roots are
generated once and kept off networked machines; delegates are cheap and
rotatable, which is why CI is given one and never a root — a leaked delegate is
revoked by issuing another and raising the version floor, whereas a leaked root
can only be replaced by visiting every deployed node.

    fw_sign.py genkey   --out keys/root-a          a keypair; roots go offline
    fw_sign.py delegate --root keys/root-a --key keys/ci --label ci \\
                        --min-version 1 --out keys/ci.delegation
    fw_sign.py sign     --image .pio/build/t3s3/firmware.bin --board t3s3 \\
                        --version v0.1.0 --secure-version 1 --slot-size 1966080 \\
                        --key keys/ci --delegation keys/ci.delegation \\
                        --out firmware.manifest
    fw_sign.py roots    --out src/sys/OtaRoots.h keys/root-a.pub keys/root-b.pub
    fw_sign.py dump     firmware.manifest

Signing is optional in a pipeline: with no key set, nothing is signed and the
artefacts are simply unsigned, which a node then refuses to install over the
air. Verification on the node is never optional.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

# Must match src/sys/FirmwareManifest.h exactly. `fixture` below exists so that
# a disagreement is a failing test rather than a node refusing every update.
MAGIC = b"RMFW"
FORMAT_VERSION = 1
PURPOSE_FIRMWARE = 1 << 0
IMAGE_RECORD_SIZE = 100
DELEGATION_RECORD_SIZE = 56
SIZE = 284
BOARD_LEN, VERSION_LEN, LABEL_LEN = 16, 32, 16


def _fixed(text: str, length: int) -> bytes:
    raw = text.encode()
    if len(raw) > length:
        sys.exit(f"'{text}' is longer than the {length} bytes the manifest allows")
    return raw.ljust(length, b"\0")


def image_record(image_hash: bytes, size: int, secure_version: int,
                 board: str, version: str, slot: int) -> bytes:
    r = bytearray(IMAGE_RECORD_SIZE)
    r[0:4] = MAGIC
    r[4] = FORMAT_VERSION
    r[8:40] = image_hash
    struct.pack_into("<I", r, 40, size)
    struct.pack_into("<I", r, 44, secure_version)
    r[48:64] = _fixed(board, BOARD_LEN)
    r[64:96] = _fixed(version, VERSION_LEN)
    struct.pack_into("<I", r, 96, slot)
    return bytes(r)


def delegation_record(delegate_pub: bytes, purpose: int, min_version: int, label: str) -> bytes:
    r = bytearray(DELEGATION_RECORD_SIZE)
    r[0:32] = delegate_pub
    struct.pack_into("<I", r, 32, purpose)
    struct.pack_into("<I", r, 36, min_version)
    r[40:56] = _fixed(label, LABEL_LEN)
    return bytes(r)


def load_private(path: Path) -> ed25519.Ed25519PrivateKey:
    return ed25519.Ed25519PrivateKey.from_private_bytes(path.read_bytes())


def cmd_genkey(args):
    key = ed25519.Ed25519PrivateKey.generate()
    priv = Path(f"{args.out}.key")
    pub = Path(f"{args.out}.pub")
    if priv.exists() and not args.force:
        sys.exit(f"{priv} exists; refusing to overwrite a key (--force to insist)")
    priv.parent.mkdir(parents=True, exist_ok=True)
    priv.write_bytes(key.private_bytes(serialization.Encoding.Raw,
                                       serialization.PrivateFormat.Raw,
                                       serialization.NoEncryption()))
    priv.chmod(0o600)
    pub.write_bytes(key.public_key().public_bytes(serialization.Encoding.Raw,
                                                  serialization.PublicFormat.Raw))
    print(f"private {priv} (keep this offline if it is a root)\npublic  {pub}")


def cmd_delegate(args):
    root = load_private(Path(args.root))
    delegate_pub = Path(f"{args.key}.pub").read_bytes()
    record = delegation_record(delegate_pub, PURPOSE_FIRMWARE, args.min_version, args.label)
    Path(args.out).write_bytes(record + root.sign(record))
    print(f"delegation for '{args.label}', floor {args.min_version} -> {args.out}")


def cmd_sign(args):
    image = Path(args.image).read_bytes()
    signed = Path(args.delegation).read_bytes()
    if len(signed) != DELEGATION_RECORD_SIZE + 64:
        sys.exit(f"{args.delegation} is not a delegation")
    record, root_sig = signed[:DELEGATION_RECORD_SIZE], signed[DELEGATION_RECORD_SIZE:]
    img = image_record(hashlib.sha256(image).digest(), len(image), args.secure_version,
                       args.board, args.version, args.slot_size)
    if len(image) > args.slot_size:
        sys.exit(f"image is {len(image)} bytes, larger than the {args.slot_size}-byte slot")
    delegate_sig = load_private(Path(f"{args.key}.key")).sign(img)
    Path(args.out).write_bytes(img + record + root_sig + delegate_sig)
    print(f"signed {args.board} {args.version} ({len(image)} bytes, "
          f"secure_version {args.secure_version}) -> {args.out}")


def cmd_roots(args):
    keys = [Path(p).read_bytes() for p in args.pubkeys]
    for k, p in zip(keys, args.pubkeys):
        if len(k) != 32:
            sys.exit(f"{p} is not a 32-byte public key")
    if len(keys) < 2:
        print("warning: fewer than two roots. One lost root cannot be replaced without "
              "reflashing every deployed node by hand.", file=sys.stderr)
    lines = ["// Generated by tools/fw_sign.py — do not edit.",
             "//",
             "// The trust anchors compiled into this firmware. Their private halves live",
             "// offline; adding one to a node that is already deployed needs a firmware",
             "// update signed by one of these, so the list is chosen once and lived with.",
             "#pragma once", "", "#include <stdint.h>", "",
             f"static const uint8_t OTA_ROOT_KEYS[{len(keys)}][32] = {{"]
    for k, p in zip(keys, args.pubkeys):
        body = ", ".join(f"0x{b:02x}" for b in k)
        lines.append(f"  {{ {body} }},  // {Path(p).stem}")
    lines += ["};", ""]
    Path(args.out).write_text("\n".join(lines))
    print(f"{len(keys)} root(s) -> {args.out}")


def cmd_dump(args):
    b = Path(args.manifest).read_bytes()
    if len(b) != SIZE or b[:4] != MAGIC:
        sys.exit("not a manifest")
    board = b[48:64].rstrip(b"\0").decode()
    version = b[64:96].rstrip(b"\0").decode()
    label = b[140:156].rstrip(b"\0").decode()
    print(f"  image      {b[8:40].hex()}")
    print(f"  size       {struct.unpack_from('<I', b, 40)[0]}")
    print(f"  board      {board}")
    print(f"  version    {version}")
    print(f"  secure_ver {struct.unpack_from('<I', b, 44)[0]}")
    print(f"  slot       {struct.unpack_from('<I', b, 96)[0]}")
    print(f"  delegate   {b[100:132].hex()} '{label}' floor "
          f"{struct.unpack_from('<I', b, 136)[0]}")


def cmd_fixture(args):
    """A manifest with known values, as a C array, so the firmware's parser and
    this file cannot drift apart without a test noticing."""
    root = ed25519.Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
    delegate = ed25519.Ed25519PrivateKey.from_private_bytes(bytes(range(32, 64)))
    dpub = delegate.public_key().public_bytes(serialization.Encoding.Raw,
                                              serialization.PublicFormat.Raw)
    record = delegation_record(dpub, PURPOSE_FIRMWARE, 5, "fixture")
    img = image_record(bytes(range(32)), 1800000, 7, "t3s3", "v9.9.9", 1966080)
    blob = img + record + root.sign(record) + delegate.sign(img)
    body = "\n".join("  " + ", ".join(f"0x{x:02x}" for x in blob[i:i + 12]) + ","
                     for i in range(0, len(blob), 12))
    Path(args.out).write_text(
        "// Generated by `tools/fw_sign.py fixture` — do not edit.\n"
        "//\n"
        "// A manifest built by the signing tool, parsed by the firmware's own reader in\n"
        "// test_firmware_manifest. It is here so that a change to either side's idea of\n"
        "// the layout fails a test rather than bricking the update path.\n"
        "#pragma once\n\n#include <stdint.h>\n\n"
        f"static const uint8_t MANIFEST_FIXTURE[{len(blob)}] = {{\n{body}\n}};\n")
    print(f"fixture ({len(blob)} bytes) -> {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("genkey"); p.add_argument("--out", required=True)
    p.add_argument("--force", action="store_true"); p.set_defaults(fn=cmd_genkey)

    p = sub.add_parser("delegate")
    p.add_argument("--root", required=True); p.add_argument("--key", required=True)
    p.add_argument("--label", required=True); p.add_argument("--min-version", type=int, default=0)
    p.add_argument("--out", required=True); p.set_defaults(fn=cmd_delegate)

    p = sub.add_parser("sign")
    for a in ("--image", "--board", "--version", "--key", "--delegation", "--out"):
        p.add_argument(a, required=True)
    p.add_argument("--secure-version", type=int, required=True)
    p.add_argument("--slot-size", type=int, required=True); p.set_defaults(fn=cmd_sign)

    p = sub.add_parser("roots"); p.add_argument("--out", required=True)
    p.add_argument("pubkeys", nargs="+"); p.set_defaults(fn=cmd_roots)

    p = sub.add_parser("dump"); p.add_argument("manifest"); p.set_defaults(fn=cmd_dump)
    p = sub.add_parser("fixture"); p.add_argument("--out", required=True); p.set_defaults(fn=cmd_fixture)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
