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

// PppUart.cpp — see PppUart.h for the shape of the thing. Four parts:
//
//   1. The port: the console's view of it, and how the log is silenced
//      while PPP owns the port.
//   2. The network interface: an esp_netif on the PPP stack, attached to a
//      driver whose transmit writes whole frames to the UART and whose
//      receive path is the reader below.
//   3. The event handlers, which record and nothing more.
//   4. The reader: one task that blocks on the UART driver, hands bytes to
//      the arbiter while the console owns the port and to lwIP while PPP
//      does, and carries out every change of ownership.
//
// Every esp_netif action — start, stop — runs on the reader task, so the
// session's whole life is on one task; the loop task only sets the switch
// and reads the mirrors, and the event handlers, on the system event task,
// only set flags the reader acts on at its next pass.
#include "PppUart.h"

#if HAS_PPP
#include <atomic>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_defaults.h>
#include <esp_netif_ppp.h>
#include <esp_netif_net_stack.h>
#include <esp_event.h>
#include <esp_log.h>
#include <driver/uart.h>
#include <lwip/netif.h>
#include <netif/ppp/ppp.h>
#include <netif/ppp/ipcp.h>
#include "LocalLinkState.h"
#include "LocalLink.h"
#include "RnsTransport.h"

using LocalLink::pppNodeAddress;

namespace PppUart {

static const uart_port_t kUart = (uart_port_t)BOARD_UART_INSTANCE;

// The flow-control hooks. No board here routes the bridge's RTS/CTS to the
// chip (boards.json uart.rts/cts), so neither is driven; a board that did
// would name its pins here and begin() would set UART_HW_FLOWCTRL_CTS_RTS
// on the driver.
static constexpr int kPinRts = -1;
static constexpr int kPinCts = -1;

// How long a session may take to end after the reader asks: the two
// Terminate-Requests lwIP sends three seconds apart, and a margin. A peer
// that has stopped answering never acknowledges, and the port must not
// stay PPP's for ever on its account.
static constexpr uint32_t kStopGraceMs = 10000;

// ---------------------------------------------------------------------------
// State. The reader task owns the arbiter and the session; everything the
// loop task, the console and the event task read or set is atomic.
// ---------------------------------------------------------------------------
static esp_netif_t*          sNetif = nullptr;
static Arbiter               sArbiter(false);           // reader task only
static std::atomic<Owner>    sOwner{Owner::Console};    // the arbiter's answer, for other tasks
static std::atomic<bool>     sEnabled{false};           // the operator's switch, as last applied
static std::atomic<bool>     sClosing{false};           // the node is restarting: end the session
static std::atomic<bool>     sStarted{false};           // connect was asked of esp_netif; transmit allowed
static std::atomic<bool>     sUp{false};                // IPCP done: address and peer are valid
static std::atomic<bool>     sDead{false};              // the session reached PPP's dead phase
static std::atomic<uint32_t> sOurIp{0}, sPeerIp{0};     // host order
static std::atomic<uint32_t> sBaud{PPP_BAUD_DEFAULT};
static std::atomic<uint32_t> sRx{0}, sTx{0}, sTxDropped{0}, sSessions{0};
static uint8_t               sMacLastOctet = 0;

static IPAddress ipOf(uint32_t hostOrder) {
  return IPAddress(hostOrder >> 24, hostOrder >> 16, hostOrder >> 8, hostOrder);
}

// ---------------------------------------------------------------------------
// 1. The port
// ---------------------------------------------------------------------------
// Bytes for the console, from the reader task to the loop task: one
// producer, one consumer, no lock. A console command is a dozen bytes and a
// host pasting a script is under a hundred, so the ring is small, and the
// overflow policy is the UART driver's — the newest byte is dropped — which
// never fires unless something floods the port, and that is not a console.
class ConsoleStream : public Stream {
public:
  static constexpr size_t kSize = 256;

  void push(const uint8_t* data, size_t len) {
    size_t head = _head.load(std::memory_order_relaxed);
    const size_t tail = _tail.load(std::memory_order_acquire);
    for (size_t i = 0; i < len; i++) {
      const size_t next = (head + 1) % kSize;
      if (next == tail) break;                          // full: drop the rest
      _buf[head] = data[i];
      head = next;
    }
    _head.store(head, std::memory_order_release);
  }

  int available() override {
    const size_t head = _head.load(std::memory_order_acquire);
    const size_t tail = _tail.load(std::memory_order_relaxed);
    return (int)((head + kSize - tail) % kSize);
  }
  int peek() override {
    if (!available()) return -1;
    return _buf[_tail.load(std::memory_order_relaxed)];
  }
  int read() override {
    if (!available()) return -1;
    const size_t tail = _tail.load(std::memory_order_relaxed);
    const uint8_t c = _buf[tail];
    _tail.store((tail + 1) % kSize, std::memory_order_release);
    return c;
  }
  // The console's replies. While PPP owns the port they would land inside
  // frames — and nobody is waiting for them there: the console received
  // no command, since its bytes went to PPP. Dropped, and reported as sent.
  size_t write(uint8_t c) override {
    return sOwner.load(std::memory_order_relaxed) == Owner::Console ? Serial.write(c) : 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    return sOwner.load(std::memory_order_relaxed) == Owner::Console ? Serial.write(data, len) : len;
  }
  void flush() override {
    if (sOwner.load(std::memory_order_relaxed) == Owner::Console) Serial.flush();
  }

private:
  uint8_t _buf[kSize];
  std::atomic<size_t> _head{0}, _tail{0};
};
static ConsoleStream sConsole;

Stream& console() { return sConsole; }

// The log has three sources and three mutes. The core's log_* macros go
// through ets_printf and the putc the core installed, which writes straight
// into the UART FIFO — Serial.setDebugOutput(false) uninstalls it. ESP-IDF's
// own ESP_LOG* go through vprintf, replaced here with one that writes
// nothing. microReticulum prints through Serial from the rns task, and only
// that task may touch it, so it is asked to raise its level at its next
// pass (RnsTransport::muteLog). Anything a library prints through stdout
// directly is not caught; nothing in this firmware does.
static vprintf_like_t sLogSink = nullptr;
static int silent(const char*, va_list) { return 0; }

static void muteLog(bool mute) {
  if (mute) {
    Serial.setDebugOutput(false);
    sLogSink = esp_log_set_vprintf(silent);
    RnsTransport::muteLog(true);
  } else {
    if (sLogSink) esp_log_set_vprintf(sLogSink);
    Serial.setDebugOutput(true);
    RnsTransport::muteLog(false);
  }
}

// ---------------------------------------------------------------------------
// 2. The network interface
// ---------------------------------------------------------------------------
// lwIP's PPPoS hands the escaped frame to the driver in pieces — the pbufs
// it built it in — and a log line or a console reply written between two
// pieces would corrupt the frame. So the pieces are gathered here and the
// frame leaves in one write once its closing flag has arrived: the driver
// serialises writers, and nothing can land inside it. A frame the queue
// cannot take is dropped whole, and so is one longer than the largest
// escaped 1500-byte packet, which cannot occur with the MRU lwIP
// negotiates. This runs on the TCP/IP task, which carries the portal and
// the transport too: a full queue is waited on briefly — kTxWaitMs, below,
// which is where PPP's backpressure belongs — but never indefinitely,
// because a host that has stopped draining the bridge must not stall the
// radio.
static constexpr size_t kFrameMax = 3200;
static uint8_t sFrame[kFrameMax];
static size_t  sFrameLen = 0;
static bool    sFrameOverflow = false;
static constexpr uint8_t kFlag = 0x7E;

// How long a frame may wait for room in the UART's transmit ring before it
// is dropped: two frames' worth at the slowest speed, in small steps.
static constexpr uint32_t kTxWaitMs     = 300;
static constexpr uint32_t kTxWaitStepMs = 5;

static esp_err_t transmit(void*, void* buffer, size_t len) {
  if (!sStarted || !len) return ESP_FAIL;
  const uint8_t* b = static_cast<const uint8_t*>(buffer);
  if (!sFrameOverflow) {
    if (sFrameLen + len > kFrameMax) sFrameOverflow = true;
    else { memcpy(sFrame + sFrameLen, b, len); sFrameLen += len; }
  }
  // A piece that ends in a flag ends the frame — unless it is the opening
  // flag on its own, which no piece from lwIP is, but is cheap to rule out.
  const bool ends = b[len - 1] == kFlag && (sFrameLen > 1 || sFrameOverflow);
  if (!ends) return ESP_OK;
  // The line is slow — a frame takes 130 ms at 115200 — and lwIP hands
  // frames over faster than that during any transfer. Dropping the moment
  // the ring is full cost seven frames in a 40 KB page and TCP paid for each
  // with a retransmission timeout: 22 kbit/s on a 115 kbit/s line. So a full
  // ring is waited on, briefly and on this task (the TCP/IP task, which is
  // where PPP's own backpressure belongs), and only a ring that stays full
  // for kTxWaitMs — a host that has stopped reading — drops the frame.
  size_t room = 0;
  bool sent = false;
  if (!sFrameOverflow) {
    for (uint32_t waited = 0; waited <= kTxWaitMs; waited += kTxWaitStepMs) {
      if (uart_get_tx_buffer_free_size(kUart, &room) == ESP_OK && room >= sFrameLen) {
        uart_write_bytes(kUart, sFrame, sFrameLen);
        sTx += sFrameLen;
        sent = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(kTxWaitStepMs));
    }
  }
  if (!sent) sTxDropped++;
  sFrameLen = 0;
  sFrameOverflow = false;
  return ESP_OK;
}

struct Driver { esp_netif_driver_base_t base; };

static esp_err_t postAttach(esp_netif_t* netif, esp_netif_iodriver_handle h) {
  esp_netif_driver_ifconfig_t cfg = {};
  cfg.handle = h;
  cfg.transmit = transmit;
  cfg.transmit_wrap = nullptr;
  // PPPoS copies what it is given into its own pbuf; there is nothing of
  // ours to free once a received chunk has been consumed.
  cfg.driver_free_rx_buffer = nullptr;
  static_cast<Driver*>(h)->base.netif = netif;
  return esp_netif_set_driver_config(netif, &cfg);
}

static Driver sDriver = { { postAttach, nullptr } };

// ---------------------------------------------------------------------------
// 3. The event handlers, on the system event task
// ---------------------------------------------------------------------------
static void onGotIp(void*, esp_event_base_t, int32_t, void* data) {
  const auto* e = static_cast<ip_event_got_ip_t*>(data);
  if (!e || e->esp_netif != sNetif) return;
  sOurIp  = ntohl(e->ip_info.ip.addr);
  sPeerIp = ntohl(e->ip_info.gw.addr);              // on a point-to-point link the gateway is the peer
  sUp = true;
}

static void onLostIp(void*, esp_event_base_t, int32_t, void* data) {
  const auto* e = static_cast<ip_event_got_ip_t*>(data);
  if (e && e->esp_netif != sNetif) return;
  sUp = false;
  sOurIp = 0;
  sPeerIp = 0;
}

static void onStatus(void*, esp_event_base_t, int32_t id, void*) {
  // The dead phase is where every session ends up, whoever ended it: the
  // peer's Terminate-Request, the operator's switch, or the retries after
  // a peer that stopped answering. That is the reader's cue.
  if (id == NETIF_PPP_PHASE_DEAD) sDead = true;
}

// ---------------------------------------------------------------------------
// 4. The reader
// ---------------------------------------------------------------------------
static bool     sStopping = false;                    // reader task only
static uint32_t sStopAskedMs = 0;

// PPP has taken the port. The log is silenced first and the FIFO left to
// drain, so the tail of a log line is on the wire before the first frame;
// then lwIP is asked to connect, which sends the node's own
// Configure-Request through transmit().
static void takeOver() {
  sOwner = Owner::Ppp;
  sSessions++;
  muteLog(true);
  uart_wait_tx_done(kUart, pdMS_TO_TICKS(200));
  if (sNetif && !sStarted) {
    sFrameLen = 0;
    sFrameOverflow = false;
    sStarted = true;
    esp_netif_action_start(sNetif, nullptr, 0, nullptr);
  }
}

// The session is over, one way or another: the console owns the port again
// and the log resumes. The first line it prints says what happened, which
// is the one line an operator watching the port wants.
static void finish(const char* why) {
  sStarted = false;
  sStopping = false;
  sUp = false;
  sOurIp = 0;
  sPeerIp = 0;
  sArbiter.pppDown();
  sOwner = Owner::Console;
  muteLog(false);
  log_i("ppp0: session ended (%s); the console has the port again", why);
}

static void deliver(const Route& r) {
  if (r.sink == Sink::Console && r.len) sConsole.push(r.data, r.len);
  else if (r.sink == Sink::Ppp && r.len && sStarted) {
    esp_netif_receive(sNetif, const_cast<uint8_t*>(r.data), r.len, nullptr);
    sRx += r.len;
  }
}

// Every change of state the session can make, once per pass of the reader.
static void service(uint32_t nowMs) {
  const bool enabled = sEnabled && !sClosing;
  sArbiter.allowPpp(enabled && sNetif != nullptr);
  if (sArbiter.owner() != Owner::Ppp) { sDead = false; return; }
  const bool stop = !enabled || sArbiter.pppIdleDead(nowMs);
  if (stop && !sStopping) {
    // A Terminate-Request to the peer, then the dead phase when it answers
    // or when lwIP gives up on it.
    sStopping = true;
    sStopAskedMs = nowMs;
    esp_netif_action_stop(sNetif, nullptr, 0, nullptr);
  }
  if (sDead.exchange(false)) {
    finish(!sStopping ? "the host closed it" : sClosing ? "the node is restarting"
           : !enabled ? "switched off" : "no frame from the host for 30 s");
  } else if (sStopping && nowMs - sStopAskedMs > kStopGraceMs) {
    finish("the host did not answer the close");
  }
}

static void readerTask(void*) {
  static uint8_t buf[256];
  for (;;) {
    // The driver returns early only once the buffer is full, so the wait
    // is the latency a short frame pays: ten milliseconds, a hundred idle
    // wake-ups a second when nothing arrives, each of which costs nothing.
    const int n = uart_read_bytes(kUart, buf, sizeof(buf), pdMS_TO_TICKS(10));
    const uint32_t now = millis();
    service(now);
    if (n <= 0) { deliver(sArbiter.idle(now)); continue; }
    if (sArbiter.owner() == Owner::Ppp) {
      sArbiter.pppReceived(buf, (size_t)n, now);
      if (sStarted) { esp_netif_receive(sNetif, buf, (size_t)n, nullptr); sRx += (uint32_t)n; }
      continue;
    }
    for (int i = 0; i < n; i++) {
      const Route r = sArbiter.feed(buf[i], now);
      if (r.tookOver) takeOver();
      deliver(r);
      if (r.tookOver) {
        // The rest of this chunk arrived after the byte that decided: PPP's.
        const size_t rest = (size_t)(n - i - 1);
        if (rest) {
          sArbiter.pppReceived(buf + i + 1, rest, now);
          if (sStarted) { esp_netif_receive(sNetif, buf + i + 1, rest, nullptr); sRx += rest; }
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
void begin() {
  static bool begun = false;
  if (begun) return;
  begun = true;
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  sMacLastOctet = mac[5];

  // The stack's own PPP client shape: key PPP_DEF, the PPP IP events, route
  // priority 20 — the same as usb0's, below the station uplink's.
  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
  sNetif = esp_netif_new(&cfg);
  if (!sNetif) {
    log_e("ppp0: esp_netif_new failed; the port stays the console's");
  } else {
    esp_netif_attach(sNetif, &sDriver);
    esp_netif_ppp_config_t pc = {};
    pc.ppp_phase_event_enabled = true;
    pc.ppp_error_event_enabled = true;
    esp_netif_ppp_set_params(sNetif, &pc);
    // What the node asks for in IPCP. esp_netif's public PPP configuration
    // carries addresses only in server builds, so the request goes to the
    // pcb lwIP keeps in the netif's state — the pointer pppos_create
    // stored there — and accept_local is set beside it so that a host
    // which assigns something else is obeyed rather than argued with until
    // LCP gives up. No DNS from the peer either: the node resolves nothing
    // over this link and must not let pppd rewrite its resolver.
    struct netif* impl = static_cast<struct netif*>(esp_netif_get_netif_impl(sNetif));
    ppp_pcb* pcb = impl ? static_cast<ppp_pcb*>(impl->state) : nullptr;
    if (pcb) {
      ip4_addr_t our;
      ip4_addr_set_u32(&our, htonl(pppNodeAddress(sMacLastOctet)));
      ppp_set_ipcp_ouraddr(pcb, &our);
      pcb->ipcp_wantoptions.accept_local = 1;
      ppp_set_usepeerdns(pcb, 0);
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, onGotIp, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, onLostIp, nullptr);
    esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, onStatus, nullptr);
  }
  if (kPinRts >= 0 || kPinCts >= 0)
    uart_set_hw_flow_ctrl(kUart, UART_HW_FLOWCTRL_CTS_RTS, 64);
  // Core 0 with the network stack, below the radio (5, on core 1) and level
  // with AutoInterface: the reader moves bytes and blocks on the driver;
  // lwIP does the work on its own task.
  xTaskCreatePinnedToCore(readerTask, "ppp-uart", 4096, nullptr, 2, nullptr, 0);
  log_i("ppp0: PPP client on UART%d behind the %s bridge; asks the host's pppd for %s",
        (int)kUart, BOARD_USB_BRIDGE, ipOf(pppNodeAddress(sMacLastOctet)).toString().c_str());
}

void poll(bool enabled, uint32_t baud) {
  sEnabled = enabled;
  // The speed of the whole port while PPP is on; the console's otherwise.
  // Applied while the console owns the port, never under a session — a
  // change saved over ppp0 would otherwise cut the reply that reports it —
  // and only a speed the board is qualified for, since the stored value may
  // have been written by a build with another ladder.
  const uint32_t want = enabled && LocalLink::pppBaudUsable(baud) ? baud : PPP_BAUD_DEFAULT;
  if (want != sBaud && sOwner == Owner::Console) {
    log_i("ppp0: the serial port now runs at %lu baud", (unsigned long)want);
    Serial.flush();
    Serial.updateBaudRate(want);
    sBaud = want;
  }
}

void shutdown(uint32_t waitMs) {
  if (sOwner != Owner::Ppp) return;
  // The reader does the closing; this only asks and waits. The host's pppd
  // gets a Terminate-Request and answers within milliseconds, which is
  // what lets the flashing tool see the port freed at once rather than
  // after its echo failures.
  sClosing = true;
  const uint32_t started = millis();
  while (sOwner == Owner::Ppp && millis() - started < waitMs) delay(10);
}

Owner     owner()     { return sOwner; }
bool      linkUp()    { return sUp; }
IPAddress address()   { return ipOf(sOurIp); }
IPAddress peer()      { return ipOf(sPeerIp); }
IPAddress askedAddress() { return ipOf(pppNodeAddress(sMacLastOctet)); }
IPAddress askedPeer()    { return ipOf(LocalLink::pppHostAddress(sMacLastOctet)); }
uint32_t  baud()      { return sBaud; }
uint32_t  rxBytes()   { return sRx; }
uint32_t  txBytes()   { return sTx; }
uint32_t  txDropped() { return sTxDropped; }
uint32_t  sessions()  { return sSessions; }

} // namespace PppUart
#endif
