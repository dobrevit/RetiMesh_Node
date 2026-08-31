#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Generate LXMF test vectors from the reference library.

Every message this node has ever failed on was a shape we did not think to
build by hand. The tests in test_lxmf/ construct their own buffers, so they
encode what we believed the format was — which is why a stamped message and an
opportunistic one both passed a green suite while failing on the bench.

These vectors come from LXMF itself instead. For each one the generator
records the bytes as they arrive at the delivery destination, and the
hashed_part the *library* computes over them. A parser that reproduces those
bytes will verify the sender's signature; one that does not, cannot. That
makes the assertion checkable natively, with no crypto in the test.

    pip install rns lxmf
    python tools/lxmf_vectors.py > test/test_lxmf_vectors/vectors.h

Regenerate when LXMF changes the wire format, and read the diff when you do:
a change here is a change in what clients send us.
"""

import sys
import textwrap

import RNS
import RNS.vendor.umsgpack as msgpack
from LXMF.LXMessage import LXMessage

# Fixed keys and a fixed clock, so regenerating produces the same file unless
# the wire format actually moved. With random identities every hash, signature
# and stamp changed on every run, so the diff was entirely noise and a real
# change to what clients send would have been invisible in it — which is the
# one thing the instruction above asks the diff to show.
# sha512("retimesh-lxmf-vector-source") and ...-destination, so the keys are
# re-derivable rather than magic. Test material only; nothing signs with these
# but this generator.
SRC_KEY = bytes.fromhex(
    "fb8d78312fe9b4516317f512ff32f4a9bf3e4a18684bdfe4ffd99a8c39f4be72"
    "db9b83eee5822bcd2f20572ec90e6e80616a748cc7ed63d5b77cac56c1dc972d")
DST_KEY = bytes.fromhex(
    "5096c2467d1caa7a1b4c92dac9d9b5f8341bffb195fc67bb5474102540f99328"
    "3f331dfaf2f9c0799cf6d4958564cde91ea0cc6fbd9d0d32e81f09d19b1dbe8d")
TIMESTAMP = 1767225600.0        # 2026-01-01T00:00:00Z, so packing is deterministic


def build(dst, src, content, title="", fields=None, stamp_cost=None, opportunistic=False):
    m = LXMessage(dst, src, content, title, fields=fields or {},
                  desired_method=LXMessage.DIRECT)
    m.stamp_cost = stamp_cost
    m.defer_stamp = False           # what LXMRouter does once a stamp exists
    m.timestamp = TIMESTAMP         # so the same input yields the same bytes
    if stamp_cost is not None:
        # A stamp is a search for a random value, so generating one would make
        # these vectors differ on every run. At cost 0 every value is valid —
        # which is the whole reason announcing 0 is a defect — so a fixed one
        # is as real as a searched one and keeps the diff meaningful.
        m.stamp = bytes(range(32))
    m.pack()

    packed = m.packed
    # LXMF strips the destination hash when it sends a message as a single
    # packet, because the receiver is the destination. The receiving router
    # puts it back before parsing.
    wire = packed[16:] if opportunistic else packed

    # What the library signs: the payload with anything past the fourth
    # element dropped and the remainder re-packed.
    payload = msgpack.unpackb(packed[96:])
    stamped = len(payload) > 4
    hashed = packed[:16] + packed[16:32] + msgpack.packb(payload[:4])
    assert hashed == (packed[:32] + packed[96:]) or stamped, \
        "unstamped hashed_part should be the payload as received"

    return {
        "wire": wire, "dest": packed[:16], "opportunistic": opportunistic,
        "hashed": hashed, "content": content, "title": title, "stamped": stamped,
    }


def carray(name, data):
    body = "\n".join(textwrap.wrap(", ".join(f"0x{b:02X}" for b in data), 88))
    return f"static const uint8_t {name}[] = {{\n" + textwrap.indent(body, "  ") + "\n};\n"


def cstring(s):
    # C hex escapes are maximal-munch: "\xc3\xa91" parses \xa91 as one escape
    # and fails to compile. Closing and reopening the literal after every
    # escape stops the next character being swallowed into it, which matters
    # the moment anyone adds a vector whose text is not ASCII — the reason
    # this branch exists.
    out = ""
    escaped = False
    for ch in s.encode("utf-8"):
        if ch == 0x22:
            out += '\\"'; escaped = False
        elif ch == 0x5C:
            out += "\\\\"; escaped = False
        elif 0x20 <= ch < 0x7F:
            if escaped and chr(ch) in "0123456789abcdefABCDEF":
                out += '" "'
            out += chr(ch); escaped = False
        else:
            out += f"\\x{ch:02x}"; escaped = True
    return f'"{out}"'


# The announces LXMF itself emits, which is the half the message vectors do
# not cover — and the gap that let this node announce its name as a msgpack
# str for a release. LXMF packs the name with .encode("utf-8"), so it goes out
# as bin and comes back through .decode("utf-8"); a str announce unpacks to a
# Python str, .decode raises, and the client shows no name at all. Our own
# reader accepts either, so a round-trip test against ourselves cannot see it.
# These bytes come from LXMRouter, so it can.
ANNOUNCE_CASES = [
    ("ascii", "retimesh-52A7F8"),
    ("utf8",  "Café Мартин"),
    ("empty", ""),
]


def announce_app_data(display_name):
    """Byte for byte what LXMRouter.get_announce_app_data() produces."""
    from LXMF.LXMF import SF_COMPRESSION
    name = display_name.encode("utf-8") if display_name else None
    return msgpack.packb([name, None, [SF_COMPRESSION]])


def main():
    RNS.Reticulum(sys.argv[1] if len(sys.argv) > 1 else None)
    src_id = RNS.Identity.from_bytes(SRC_KEY)
    dst_id = RNS.Identity.from_bytes(DST_KEY)
    src = RNS.Destination(src_id, RNS.Destination.IN, RNS.Destination.SINGLE, "lxmf", "delivery")
    dst = RNS.Destination(dst_id, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")

    cases = [
        ("plain",          dict(content="hello from a real client")),
        ("titled",         dict(content="body text here", title="Subject line")),
        ("stamped",        dict(content="a message with a stamp", stamp_cost=0)),
        ("opportunistic",  dict(content="short one", opportunistic=True)),
        ("opportunistic_stamped",
                           dict(content="short and stamped", stamp_cost=0, opportunistic=True)),
        ("attachment",     dict(content="photo attached",
                                fields={0x06: [b"image/jpeg", b"\xff\xd8\xff\xe0" * 20]})),
        ("stamped_fields", dict(content="both at once",
                                fields={0x08: b"\x11" * 32}, stamp_cost=0)),
        ("long",           dict(content="The repeater on the hill is back up after the storm. " * 8)),
        ("empty_title",    dict(content="no subject", title="")),
    ]

    vectors = [(tag, build(dst, src, **kw)) for tag, kw in cases]

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.")
    out.append("//")
    out.append("// GENERATED by tools/lxmf_vectors.py from the reference LXMF library.")
    out.append(f"// LXMF {__import__('LXMF')._version.__version__}, "
               f"RNS {RNS.__dict__.get('__version__', 'unknown')}. Do not edit by hand.")
    out.append("#pragma once")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    out.append("")
    for tag, v in vectors:
        out.append(carray(f"kWire_{tag}", v["wire"]))
        out.append(carray(f"kHashed_{tag}", v["hashed"]))
        if v["opportunistic"]:
            out.append(carray(f"kDest_{tag}", v["dest"]))
    out.append("struct LxmfVector {")
    out.append("  const char*    tag;")
    out.append("  const uint8_t* wire;   size_t wireLen;")
    out.append("  const uint8_t* dest;                  // prepend for an opportunistic vector, else null")
    out.append("  const uint8_t* hashed; size_t hashedLen;   // what the library signed")
    out.append("  const char*    content;")
    out.append("  const char*    title;")
    out.append("  bool           stamped;")
    out.append("};")
    out.append("")
    out.append("static const LxmfVector kLxmfVectors[] = {")
    for tag, v in vectors:
        dest = f"kDest_{tag}" if v["opportunistic"] else "nullptr"
        out.append(f'  {{ "{tag}", kWire_{tag}, sizeof(kWire_{tag}), {dest},')
        out.append(f'    kHashed_{tag}, sizeof(kHashed_{tag}),')
        out.append(f'    {cstring(v["content"])}, {cstring(v["title"])}, '
                   f'{"true" if v["stamped"] else "false"} }},')
    out.append("};")

    # The announce direction.
    out.append("")
    for tag, name in ANNOUNCE_CASES:
        out.append(carray(f"kAnnounce_{tag}", announce_app_data(name)))
    out.append("struct LxmfAnnounceVector {")
    out.append("  const char*    tag;")
    out.append("  const uint8_t* appData; size_t appDataLen;  // as LXMRouter emits it")
    out.append("  const char*    name;                        // what a client reads back out")
    out.append("};")
    out.append("")
    out.append("static const LxmfAnnounceVector kLxmfAnnounceVectors[] = {")
    for tag, name in ANNOUNCE_CASES:
        out.append(f'  {{ "{tag}", kAnnounce_{tag}, sizeof(kAnnounce_{tag}), {cstring(name)} }},')
    out.append("};")
    print("\n".join(out))


if __name__ == "__main__":
    main()
