// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  RnsTransport.cpp — spike: instantiate microReticulum with transport on.
// ============================================================================
#include "RnsTransport.h"
#include <microReticulum.h>
#include "RnsFileSystem.h"
#include "RnsAnnounce.h"

static RNS::Reticulum reticulum({RNS::Type::NONE});
// microStore view of the LittleFS partition we already mount in setup(),
// with microReticulum's relative paths mapped under /rns (see RnsFileSystem.h).
static RnsFileSystem rnsFs;

namespace RnsTransport {

static bool started = false;

bool begin() {
  try {
    RNS::loglevel(RNS::LOG_INFO);
    rnsFs.init(false);
    RNS::Utilities::OS::register_filesystem(rnsFs);
    reticulum = RNS::Reticulum();          // ctor resets the storage path, so set it after
    RNS::Reticulum::storagepath(RNS_FS_ROOT);
    reticulum.transport_enabled(true);

    // One identity for the node: the transport signs with the same keys
    // that announce retimesh.node, so Transport::identity() == nodeIdentity.
    uint8_t prv[64];
    nodeIdentity.privateKey(prv);
    RNS::Transport::set_identity_prv(RNS::Bytes(prv, sizeof(prv)));
    reticulum.start();
    started = true;
    log_i("microReticulum transport started, identity %s", RNS::Transport::identity().hash().toHex().c_str());
  } catch (const std::exception& e) {
    log_e("microReticulum start failed: %s", e.what());
  }
  return started;
}

void loop() {
  if (!started) { vTaskDelay(pdMS_TO_TICKS(1000)); return; }
  try { reticulum.loop(); } catch (const std::exception& e) { log_e("microReticulum loop: %s", e.what()); }
}

size_t pathCount() { return RNS::Transport::path_table().size(); }

}
