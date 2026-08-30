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

#include "CaptiveDns.h"
#include <Arduino.h>
#include <lwip/def.h>
#include "LocalLink.h"

// RFC 1035, the parts a captive portal needs: a header, one question, and
// one answer that points the name at us.
namespace {
constexpr size_t   kHeader   = 12;
constexpr uint8_t  kQr       = 0x80;     // flags, first byte: response
constexpr uint8_t  kAa       = 0x04;     // authoritative, which for the portal we are
constexpr uint8_t  kRcodeRefused = 5;   // flags, second byte: not for this link
constexpr uint16_t kTypeA    = 1;
constexpr uint16_t kClassIn  = 1;
constexpr size_t   kMaxReply = 512;      // a UDP DNS message; the question is bounded by the packet
}

bool CaptiveDns::begin(const IPAddress& ip, uint32_t ttlSeconds) {
  _ip = ip;
  _ttl = ttlSeconds;
  _udp.onPacket([this](AsyncUDPPacket& pkt) { handle(pkt); });
  return _udp.listen(53);
}

void CaptiveDns::end() { _udp.close(); }

void CaptiveDns::handle(AsyncUDPPacket& pkt) {
  const uint8_t* q = pkt.data();
  const size_t   n = pkt.length();
  // Header, then a name of at least one byte (the terminator), a type and a class.
  if (n < kHeader + 1 + 4 || (q[2] & kQr)) return;              // short, or not a query
  const uint16_t qdcount = (q[4] << 8) | q[5];
  if (qdcount != 1) return;
  // The question's name ends at its zero label; type and class follow it.
  size_t end = kHeader;
  while (end < n && q[end]) end += q[end] + 1;
  if (end >= n) return;                                          // no terminator: malformed
  end++;                                                         // past the terminator
  if (end + 4 > n) return;
  const uint16_t qtype  = (q[end] << 8) | q[end + 1];
  const uint16_t qclass = (q[end + 2] << 8) | q[end + 3];
  const size_t question = end + 4 - kHeader;
  // Only the access point is steered to the portal. A query that arrived
  // over any other link — the USB cable, whose lease names us as DNS — is
  // refused outright, so the resolver moves on without waiting.
  //
  // Both addresses decide that, for the reason LocalLink::requestIsHostFacing
  // is written the way it is: lwIP accepts a packet for any of the node's
  // addresses on whichever interface it arrives, so a host on the LAN or on
  // the USB link that sends to the access point's address reaches this
  // handler with the access point's address as the local one. The sender has
  // to be on the access point's own subnet as well.
  const bool ours   = pkt.localIP() == _ip &&
                      LocalLink::requestIsOnItsLink(LocalLink::hostOrder(pkt.localIP()),
                                                    LocalLink::hostOrder(pkt.remoteIP()));
  const bool answer = ours && qtype == kTypeA && qclass == kClassIn;

  uint8_t r[kMaxReply];
  // Room for the question as asked, and for the answer only when there is one
  // to write. Reserving the answer's 16 bytes on the REFUSED path too meant a
  // long name got no reply at all — the resolver then waits out its timeout,
  // which is the one thing this responder exists to prevent.
  if (kHeader + question + (answer ? 16 : 0) > sizeof(r)) return;
  memcpy(r, q, kHeader + question);                              // id, flags, counts, the question as asked
  r[2] = (q[2] & 0x01) | kQr | (ours ? kAa : 0);                 // response, recursion as asked, authoritative on the AP
  r[3] = ours ? 0 : kRcodeRefused;                               // no error, or REFUSED; no recursion available
  r[6] = 0; r[7] = answer ? 1 : 0;                               // one answer, or none for a type we do not serve
  r[8] = r[9] = r[10] = r[11] = 0;                               // no authority, no additional
  size_t len = kHeader + question;
  if (answer) {
    const uint32_t ttl = htonl(_ttl);
    const uint32_t ip  = (uint32_t)_ip;                          // network order, as IPAddress keeps it
    const uint8_t rr[] = { 0xC0, 0x0C,                           // the name, by pointer to the question
                           0, kTypeA, 0, kClassIn };
    memcpy(r + len, rr, sizeof(rr));           len += sizeof(rr);
    memcpy(r + len, &ttl, 4);                  len += 4;
    r[len++] = 0; r[len++] = 4;                                  // RDLENGTH
    memcpy(r + len, &ip, 4);                   len += 4;
  }
  _udp.writeTo(r, len, pkt.remoteIP(), pkt.remotePort());
}
