// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.


// ============================================================================
//  UbxFrame.h — one binary frame for a u-blox receiver, and only one
//
//  This firmware's GNSS driver is NMEA and nothing else: it assembles
//  sentences, checks their XOR and matches them by type. It has never sent a
//  receiver anything. One board makes that untenable — the T-Deck's MIA-M10Q
//  has no enable line, no standby line and no switched rail, so the only way
//  to stop it drawing is to ask it in its own protocol. That is the whole
//  reason this file exists, and it deliberately stops at the one message:
//  there is no parser here, no acknowledgement handling and no configuration
//  writing, because none of those are needed to let a receiver sleep.
//
//  Everything below is arithmetic over bytes — no port, no clock, no Arduino
//  — so the frame a receiver would be handed is pinned on the host
//  (test/test_ubx_frame) against the numbers in the specification, rather
//  than being discovered by watching a module fail to answer.
//
//  The source, quoted rather than remembered
//  -----------------------------------------
//  u-blox M10 SPG 5.10 Interface description, UBX-21035062 R03 (27-Jun-2023),
//  which is the firmware generation of the module this is written for.
//
//  §3.2 "UBX frame structure":
//    "Every frame starts with a 2-byte preamble consisting of two
//     synchronization characters: 0xb5 and 0x62." — "A 1-byte message class
//     field follows." — "A 1-byte message ID field defines the message that is
//     to follow." — "A 2-byte length field follows. The length is defined as
//     being that of the payload only. It does not include the preamble,
//     message class, message ID, length, or UBX checksum fields. The number
//     format of the length field is an unsigned little-endian 16-bit integer"
//     — "The two 1-byte CK_A and CK_B fields hold a 16-bit checksum".
//
//  §3.4 "UBX checksum":
//    "The checksum is calculated over the message, starting and including the
//     class field up until, but excluding, the checksum fields." — "The
//     checksum algorithm used is the 8-bit Fletcher algorithm, which is used
//     in the TCP standard RFC 1145)", given as:
//       CK_A = 0, CK_B = 0
//       For (I = 0; I < N; I++) { CK_A = CK_A + Buffer[I]; CK_B = CK_B + CK_A }
//     with both held to eight bits.
//
//  §3.16.6 "UBX-RXM-PMREQ (0x02 0x41)", "Power management request", type
//  Command, "This message requests a power management related task of the
//  receiver." Its message structure line reads `0xb5 0x62  0x02  0x41  16`,
//  and the payload is:
//       0   U1     version        "Message version (0x00 for this version)"
//       1   U1[3]  reserved0      "Reserved"
//       4   U4     duration       "Duration of the requested task. The maximum
//                                  supported value is 12 days. Set to 0 to
//                                  wait for a wakeup signal on a pin"
//       8   X4     flags          bit 1 backup "Set to 1 to put the receiver
//                                  into backup mode"
//                                 bit 2 force  "Set to 1 for minimum power
//                                  consumption"
//      12   X4     wakeupSources  "Configure pins to wake up the receiver. The
//                                  receiver wakes up if there is either a
//                                  falling or a rising edge on one of the
//                                  configured pins."
//                                 bit 3 uartrx "Wake up the receiver if there
//                                  is an edge on the UART RX pin"
//                                 bit 5 extint0, bit 6 extint1, bit 7 spics
//
//  Note the bit numbers: uartrx is bit **3** and extint0 is bit **5**, which
//  is not the order they are listed in and is exactly the kind of constant
//  this file exists to stop anyone quoting from memory. They are quoted from
//  the section above and nowhere else — no contrast with another generation is
//  offered, because there is none to draw: the same numbering appears in the
//  M8 tables, and inventing a difference would only invite a later reader to
//  "correct" one of them. §3.3.2 also settles the reserved field: reserved
//  elements "must be set to zero in input messages".
//
//  What could not be established here, and is a bench question
//  -----------------------------------------------------------
//   * whether the module on a given board answers at all. §3.5.1 promises an
//     acknowledgement only for the CFG class — "Some messages from other
//     classes also use the same acknowledgement mechanism" — so nothing here
//     may assume a PMREQ is confirmed. The caller finds out by whether the
//     sentences stop, and the duty policy's own rule (no fix, no rest) is what
//     makes a receiver that ignored the message harmless rather than broken.
//   * whether the board backs V_BCKP. u-blox's own answer for this module is
//     that software backup is entered "by sending a UBX-RXM-PMREQ message",
//     and that V_BCKP must hold the backup current for the receiver to hot
//     start afterwards; a board that leaves the pin unsupplied gets a cold
//     start on every wake instead. That is a schematic question, not a
//     firmware one.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Ubx {

// §3.2. Two synchronization characters, then class, id and a little-endian
// payload length; two checksum bytes close the frame.
constexpr uint8_t kSync1 = 0xB5;
constexpr uint8_t kSync2 = 0x62;
constexpr size_t  kHeaderLen   = 6;   // sync, sync, class, id, len_lo, len_hi
constexpr size_t  kChecksumLen = 2;
constexpr size_t  kOverhead    = kHeaderLen + kChecksumLen;

// §3.16.6.
constexpr uint8_t kClassRxm   = 0x02;
constexpr uint8_t kIdPmreq    = 0x41;
constexpr size_t  kPmreqLen   = 16;   // the length the message structure states
constexpr size_t  kPmreqFrame = kOverhead + kPmreqLen;   // 24 bytes on the wire

// §3.16.6, flags. Bit 0 is not described and is therefore reserved (§3.3.2).
constexpr uint32_t kPmreqBackup = 1u << 1;
constexpr uint32_t kPmreqForce  = 1u << 2;

// §3.16.6, wakeupSources, quoted from that table.
constexpr uint32_t kWakeUartRx  = 1u << 3;
constexpr uint32_t kWakeExtInt0 = 1u << 5;
constexpr uint32_t kWakeExtInt1 = 1u << 6;
constexpr uint32_t kWakeSpiCs   = 1u << 7;

// §3.4, verbatim: eight-bit Fletcher over class, id, both length bytes and the
// payload — everything between the preamble and the checksum itself.
inline void checksum(const uint8_t* body, size_t len, uint8_t& ckA, uint8_t& ckB) {
  ckA = 0;
  ckB = 0;
  for (size_t i = 0; i < len; i++) {
    ckA = (uint8_t)(ckA + body[i]);
    ckB = (uint8_t)(ckB + ckA);
  }
}

// Builds a whole frame into `out` and returns its length, or 0 when the buffer
// is too small — never a partial frame, because half a frame on a UART is a
// receiver hunting for a preamble that never comes. A null payload is legal
// only with len 0.
inline size_t frame(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len,
                    uint8_t* out, size_t cap) {
  if (!out) return 0;
  if (len && !payload) return 0;
  const size_t total = kOverhead + len;
  if (cap < total) return 0;
  out[0] = kSync1;
  out[1] = kSync2;
  out[2] = cls;
  out[3] = id;
  out[4] = (uint8_t)(len & 0xFF);          // little-endian, §3.2
  out[5] = (uint8_t)(len >> 8);
  for (uint16_t i = 0; i < len; i++) out[kHeaderLen + i] = payload[i];
  // From the class byte to the last payload byte, which is what §3.4 covers.
  checksum(out + 2, 4 + (size_t)len, out[kHeaderLen + len], out[kHeaderLen + len + 1]);
  return total;
}

// The one message this firmware sends: version 0 UBX-RXM-PMREQ asking for
// backup mode.
//
// `durationMs` 0 means "wait for a wakeup signal on a pin" in the
// specification's own words. The caller passes a real duration instead, and
// the reason is worth stating: the schedule genuinely does belong on the
// microcontroller, where it can be read and tested, and the wake genuinely is
// a byte on the receiver's receive line — but on the one board this is used on
// there is no other way back. If that line turns out not to be routed, or the
// module ignores the wakeup source, a duration of 0 is a receiver that never
// speaks again and a board with no rail to power-cycle it with. So the
// receiver is given the same interval the policy is holding, as a backstop: in
// the ordinary case the microcontroller's byte ends the rest first and the
// timer never runs out, and in the case nobody can test from here the node
// comes back by itself.
//
// `force` is deliberately not offered. It is documented only as "minimum power
// consumption", with no statement of what it gives up, and a flag whose cost
// is unstated is not a flag to set on a fleet.
inline size_t pmreqBackup(uint32_t durationMs, uint32_t wakeupSources,
                          uint8_t* out, size_t cap) {
  uint8_t p[kPmreqLen] = {0};              // reserved0 and the pad stay zero (§3.3.2)
  p[0] = 0x00;                             // version
  p[4] = (uint8_t)(durationMs & 0xFF);     // U4, little-endian like every UBX word
  p[5] = (uint8_t)((durationMs >> 8) & 0xFF);
  p[6] = (uint8_t)((durationMs >> 16) & 0xFF);
  p[7] = (uint8_t)((durationMs >> 24) & 0xFF);
  const uint32_t flags = kPmreqBackup;
  p[8]  = (uint8_t)(flags & 0xFF);
  p[9]  = (uint8_t)((flags >> 8) & 0xFF);
  p[10] = (uint8_t)((flags >> 16) & 0xFF);
  p[11] = (uint8_t)((flags >> 24) & 0xFF);
  p[12] = (uint8_t)(wakeupSources & 0xFF);
  p[13] = (uint8_t)((wakeupSources >> 8) & 0xFF);
  p[14] = (uint8_t)((wakeupSources >> 16) & 0xFF);
  p[15] = (uint8_t)((wakeupSources >> 24) & 0xFF);
  return frame(kClassRxm, kIdPmreq, p, (uint16_t)kPmreqLen, out, cap);
}

} // namespace Ubx
