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
//  RnsAnnounce.cpp — see RnsAnnounce.h
// ============================================================================
#include "RnsAnnounce.h"
#include "LxmfFormat.h"
#include <Preferences.h>
#include <esp_random.h>
#include <SHA256.h>
#include <Ed25519.h>
#include <Curve25519.h>

NodeIdentity nodeIdentity;

namespace Rns {

// sha256(full destination name)[:10] for aspects worth naming.
static const struct { const char* name; uint8_t hash[NAME_HASH]; } kAspects[] = {
  { "retimesh.node",     { 0xDA, 0x30, 0x6B, 0x6B, 0x52, 0xA6, 0xFA, 0xD8, 0x2D, 0xC7 } },
  { "lxmf.delivery",     { 0x6E, 0xC6, 0x0B, 0xC3, 0x18, 0xE2, 0xC0, 0xF0, 0xD9, 0x08 } },
  { "lxmf.propagation",  { 0xE0, 0x3A, 0x09, 0xB7, 0x7A, 0xC2, 0x1B, 0x22, 0x25, 0x8E } },
  { "nomadnetwork.node", { 0x21, 0x3E, 0x63, 0x11, 0xBC, 0xEC, 0x54, 0xAB, 0x4F, 0xDE } },
  { "rnstransport.probe",{ 0xFD, 0x68, 0x80, 0x5F, 0x2E, 0xA3, 0x83, 0xC8, 0xD6, 0xF6 } },
};
static const uint8_t kRetiMeshNodeNameHash[NAME_HASH] = { 0xDA, 0x30, 0x6B, 0x6B, 0x52, 0xA6, 0xFA, 0xD8, 0x2D, 0xC7 };

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  SHA256 h;
  h.update(data, len);
  h.finalize(out, 32);
}

void toHex(const uint8_t* data, size_t len, char* out) {
  static const char* d = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) { out[2*i] = d[data[i] >> 4]; out[2*i+1] = d[data[i] & 15]; }
  out[2*len] = '\0';
}

bool isAnnounce(const uint8_t* raw, size_t len) {
  if (len < 2 + HASH_LEN + 1) return false;
  return (raw[0] & 0x03) == PT_ANNOUNCE && (raw[0] & 0x80) == 0;   // no IFAC
}

bool parseAnnounce(const uint8_t* raw, size_t len, Announce& a) {
  if (!isAnnounce(raw, len)) return false;
  uint8_t flags      = raw[0];
  bool headerType2   = (flags >> 6) & 1;
  bool contextFlag   = (flags >> 5) & 1;
  size_t off = 2 + (headerType2 ? HASH_LEN : 0);
  if (len < off + HASH_LEN + 1) return false;
  a.hops     = raw[1];
  a.destHash = raw + off;              off += HASH_LEN;
  off += 1;                                           // context byte
  const uint8_t* data = raw + off;
  size_t dataLen = len - off;

  size_t need = KEY_LEN + NAME_HASH + RANDOM_LEN + (contextFlag ? RATCHET : 0) + SIG_LEN;
  if (dataLen < need) return false;
  a.publicKey  = data;
  a.nameHash   = data + KEY_LEN;
  const uint8_t* random = a.nameHash + NAME_HASH;
  const uint8_t* ratchet = contextFlag ? random + RANDOM_LEN : nullptr;
  const uint8_t* sig = (contextFlag ? ratchet + RATCHET : random + RANDOM_LEN);
  a.appData    = sig + SIG_LEN;
  a.appDataLen = dataLen - need;
  a.hasRatchet = contextFlag;

  // Destination hash must derive from name hash + identity hash.
  uint8_t full[32];
  sha256(a.publicKey, KEY_LEN, full);
  memcpy(a.identityHash, full, HASH_LEN);
  uint8_t material[NAME_HASH + HASH_LEN];
  memcpy(material, a.nameHash, NAME_HASH);
  memcpy(material + NAME_HASH, a.identityHash, HASH_LEN);
  sha256(material, sizeof(material), full);
  if (memcmp(full, a.destHash, HASH_LEN) != 0) return false;

  // Signature over dest_hash + public_key + name_hash + random + [ratchet] + app_data
  // — all contiguous in the packet except dest_hash, so assemble a copy.
  size_t signedLen = HASH_LEN + (a.appData + a.appDataLen - a.publicKey) - SIG_LEN;
  uint8_t* signedData = (uint8_t*)malloc(signedLen);
  if (!signedData) return false;
  memcpy(signedData, a.destHash, HASH_LEN);
  size_t pre = (size_t)(sig - a.publicKey);           // pub + name + random [+ ratchet]
  memcpy(signedData + HASH_LEN, a.publicKey, pre);
  memcpy(signedData + HASH_LEN + pre, a.appData, a.appDataLen);
  bool ok = Ed25519::verify(sig, a.publicKey + 32, signedData, signedLen);
  free(signedData);
  return ok;
}

const char* aspectName(const uint8_t nameHash[NAME_HASH]) {
  for (auto& k : kAspects) if (memcmp(k.hash, nameHash, NAME_HASH) == 0) return k.name;
  return nullptr;
}

static bool printable(const uint8_t* p, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++) if (p[i] < 0x20 || p[i] > 0x7E) return false;
  return true;
}

size_t displayName(const Announce& a, char* out, size_t cap) {
  const uint8_t* p = a.appData; size_t n = a.appDataLen;
  if (n == 0 || cap < 2) { out[0] = '\0'; return 0; }
  // An LXMF app_data is a msgpack array whose first element is the name; the
  // shape is LxmfFormat.h's, once, because this node emits it too.
  if (n >= 2 && (p[0] & 0xF0) == 0x90) {
    const size_t k = lxmfName(p, n, out, cap);
    return k && printable((const uint8_t*)out, k) ? k : (out[0] = '\0', 0);
  }
  if (!printable(p, n)) { out[0] = '\0'; return 0; }
  size_t k = min(n, cap - 1);
  memcpy(out, p, k); out[k] = '\0';
  return k;
}

} // namespace Rns

// ---------------------------------------------------------------------------
bool NodeIdentity::begin() {
  Preferences prefs;
  prefs.begin("retimeshid", false);
  bool have = prefs.getBytesLength("x_prv") == 32 && prefs.getBytesLength("ed_seed") == 32;
  if (have) {
    prefs.getBytes("x_prv", _xPrv, 32);
    prefs.getBytes("ed_seed", _edSeed, 32);
  } else {
    esp_fill_random(_xPrv, 32);
    esp_fill_random(_edSeed, 32);
    prefs.putBytes("x_prv", _xPrv, 32);
    prefs.putBytes("ed_seed", _edSeed, 32);
    log_w("identity: generated a new Reticulum identity");
  }
  _emitted = prefs.getUInt("ann_seq", 0);
  prefs.end();

  // X25519 public key = clamp(priv) * basepoint; Ed25519 public from the seed.
  _xPrv[0] &= 248; _xPrv[31] &= 127; _xPrv[31] |= 64;
  Curve25519::eval(_pub, _xPrv, nullptr);
  Ed25519::derivePublicKey(_pub + 32, _edSeed);

  uint8_t full[32];
  Rns::sha256(_pub, Rns::KEY_LEN, full);
  memcpy(_identityHash, full, Rns::HASH_LEN);
  uint8_t material[Rns::NAME_HASH + Rns::HASH_LEN];
  memcpy(material, Rns::kRetiMeshNodeNameHash, Rns::NAME_HASH);
  memcpy(material + Rns::NAME_HASH, _identityHash, Rns::HASH_LEN);
  Rns::sha256(material, sizeof(material), full);
  memcpy(_destHash, full, Rns::HASH_LEN);
  Rns::toHex(_destHash, Rns::HASH_LEN, _destHex);
  Rns::toHex(_identityHash, Rns::HASH_LEN, _identityHex);
  _ok = true;
  log_i("identity %s, destination retimesh.node <%s>", _identityHex, _destHex);
  return true;
}

size_t NodeIdentity::buildAnnounce(const uint8_t* appData, size_t appLen, uint8_t* out, size_t cap) {
  using namespace Rns;
  if (!_ok) return 0;
  size_t total = 2 + HASH_LEN + 1 + KEY_LEN + NAME_HASH + RANDOM_LEN + SIG_LEN + appLen;
  if (total > cap) return 0;

  // Monotonic emission stamp (no RTC): persisted counter.
  Preferences prefs;
  prefs.begin("retimeshid", false);
  _emitted = prefs.getUInt("ann_seq", 0) + 1;
  prefs.putUInt("ann_seq", _emitted);
  prefs.end();

  uint8_t random[RANDOM_LEN];
  esp_fill_random(random, 5);
  random[5] = 0;                                      // 5-byte big-endian time
  random[6] = (_emitted >> 24) & 0xFF; random[7] = (_emitted >> 16) & 0xFF;
  random[8] = (_emitted >> 8) & 0xFF;  random[9] = _emitted & 0xFF;

  // signed = dest_hash + public_key + name_hash + random + app_data
  size_t signedLen = HASH_LEN + KEY_LEN + NAME_HASH + RANDOM_LEN + appLen;
  uint8_t* signedData = (uint8_t*)malloc(signedLen);
  if (!signedData) return 0;
  uint8_t* s = signedData;
  memcpy(s, _destHash, HASH_LEN);            s += HASH_LEN;
  memcpy(s, _pub, KEY_LEN);                  s += KEY_LEN;
  memcpy(s, kRetiMeshNodeNameHash, NAME_HASH); s += NAME_HASH;
  memcpy(s, random, RANDOM_LEN);             s += RANDOM_LEN;
  memcpy(s, appData, appLen);
  uint8_t sig[SIG_LEN];
  Ed25519::sign(sig, _edSeed, _pub + 32, signedData, signedLen);
  free(signedData);

  // packet: HEADER_1, no context flag, broadcast, SINGLE, ANNOUNCE
  uint8_t* o = out;
  *o++ = 0x01;
  *o++ = 0;                                           // hops
  memcpy(o, _destHash, HASH_LEN);            o += HASH_LEN;
  *o++ = 0x00;                                        // context NONE
  memcpy(o, _pub, KEY_LEN);                  o += KEY_LEN;
  memcpy(o, kRetiMeshNodeNameHash, NAME_HASH); o += NAME_HASH;
  memcpy(o, random, RANDOM_LEN);             o += RANDOM_LEN;
  memcpy(o, sig, SIG_LEN);                   o += SIG_LEN;
  memcpy(o, appData, appLen);                o += appLen;
  return (size_t)(o - out);
}
