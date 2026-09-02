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

// UsbNcm.cpp — see UsbNcm.h for the shape of the thing. Three parts:
//
//   1. The NCM function: a descriptor handed to the core's TinyUSB glue from
//      a static initialiser, and the class callbacks TinyUSB requires.
//   2. The network interface: an esp_netif on the default Ethernet stack,
//      attached to a driver whose transmit hands frames to TinyUSB and whose
//      receive path hands TinyUSB's frames to lwIP.
//   3. The state machine in poll(): mounted, data interface active, switch
//      on — and the esp_netif actions that follow, on the loop task.
#include "UsbNcm.h"

#if HAS_USB_NCM
#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/tcpip.h>
#include <esp_netif_defaults.h>
#include <esp_timer.h>
#include <dhcpserver/dhcpserver.h>
#include <lwip/def.h>
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "device/usbd_pvt.h"              // usbd_defer_func: run a function on the USB task
#include <hal/usb_wrap_ll.h>
#include <hal/usb_serial_jtag_ll.h>
#include <soc/usb_wrap_struct.h>
#include "LocalLinkState.h"
#include "Bootloader.h"
#include "LazyStart.h"

using LocalLink::usbNodeAddress;
using LocalLink::usbHostAddress;
using LocalLink::kUsbNetmask;

#ifndef CFG_TUD_NCM
  #error "the core's TinyUSB was built without the NCM class (CONFIG_TINYUSB_NCM_ENABLED)"
#endif

namespace UsbNcm {

// ---------------------------------------------------------------------------
// Identity and addressing, derived once from the factory MAC
// ---------------------------------------------------------------------------
// The host's side of the link carries the MAC in the descriptor string; the
// node's side is the same address with the last bit flipped, as TinyUSB's
// own examples do. Both are locally administered, so neither collides with
// the Wi-Fi interfaces, which use the factory address as it is.
static uint8_t sHostMac[6];
static uint8_t sNodeMac[6];
static char    sHostMacStr[13];
static uint8_t sMacLastOctet;

static void deriveIdentity() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  memcpy(sHostMac, mac, 6);
  sHostMac[0] |= 0x02;
  memcpy(sNodeMac, sHostMac, 6);
  sNodeMac[5] ^= 0x01;
  sMacLastOctet = mac[5];
  for (int i = 0; i < 6; i++) snprintf(sHostMacStr + 2 * i, 3, "%02X", sHostMac[i]);
}

// IPAddress and esp_netif both keep the dword in network order.
IPAddress address() { return IPAddress(htonl(usbNodeAddress(sMacLastOctet))); }

// ---------------------------------------------------------------------------
// 1. The NCM function
// ---------------------------------------------------------------------------
// Full speed: 64-byte bulk endpoints, one interrupt endpoint for the
// notifications. Two IN endpoints and one OUT, beside the ACM function's two
// IN and one OUT: four IN in all, which is every IN FIFO the S3 has after
// EP0. The core's allocator refuses a fifth, and the descriptor callback
// then returns 0, which the core reports as a failed load rather than
// enumerating with an interface missing.
static constexpr uint8_t  kEndpointSize = 64;
static constexpr uint16_t kNotifyInterval = 1;
static constexpr uint8_t  kCapabilities = 0;      // no optional NCM features claimed

static uint16_t descriptor(uint8_t* dst, uint8_t* itf) {
  const uint8_t epNotify = tinyusb_get_free_in_endpoint();
  const uint8_t epIn     = tinyusb_get_free_in_endpoint();
  const uint8_t epOut    = tinyusb_get_free_out_endpoint();
  if (!epNotify || !epIn || !epOut) {
    // The core's allocator reserves an endpoint as it hands it out and offers
    // no way to hand one back, so a partial set is spent for the rest of the
    // run and the next function to ask is short of one it should have had.
    // Nothing here can undo that; what it can do is say which of the three
    // was missing, because the core reports only that an interface failed to
    // load.
    log_e("usb0: no free USB endpoint (notify %u, in %u, out %u); the network interface will not load",
          epNotify, epIn, epOut);
    return 0;
  }
  const uint8_t strName = tinyusb_add_string_descriptor(USB_NETWORK_INTERFACE);
  const uint8_t strMac  = tinyusb_add_string_descriptor(sHostMacStr);
  const uint8_t d[TUD_CDC_NCM_DESC_LEN] = {
    TUD_CDC_NCM_DESCRIPTOR(*itf, strName, strMac,
                           (uint8_t)(0x80 | epNotify), kEndpointSize,
                           epOut, (uint8_t)(0x80 | epIn), kEndpointSize,
                           CFG_TUD_NET_MTU, kNotifyInterval, kCapabilities)
  };
  *itf += 2;                                        // control + data interface
  memcpy(dst, d, sizeof(d));
  return sizeof(d);
}

// The device descriptor. The core builds its own from USB_VID/USB_PID, but
// the variant header it compiles against defines both, unconditionally,
// after any flag the build passes — so every S3 build of the core says
// 303a:1001, the serial-JTAG unit's identity, whatever platformio.ini asks.
// The callback the core hands to TinyUSB for this descriptor is weak, and
// this one replaces it: the same shape the core uses (composite, IAD,
// strings 1-3 as the core registers them), with the identity from
// boards.json.
extern "C" uint8_t const* tud_descriptor_device_cb(void) {
  static const tusb_desc_device_t d = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = RETIMESH_USB_VID,
    .idProduct          = RETIMESH_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
  };
  return reinterpret_cast<uint8_t const*>(&d);
}

// Registered before the core starts the device: with ARDUINO_USB_CDC_ON_BOOT
// the core calls USB.begin() ahead of setup(), and the descriptors are
// assembled then, once. A static initialiser is the one hook that runs
// earlier. It touches nothing but the core's C-side registry and the efuse.
namespace {
struct Registrar {
  Registrar() {
    deriveIdentity();
    tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, descriptor);
  }
};
static Registrar sRegistrar;
}

// ---------------------------------------------------------------------------
// 2. The network interface
// ---------------------------------------------------------------------------
// Atomic because the switch creates and destroys it from the loop task while
// the USB task reads it in tud_network_recv_cb and the TCP/IP task in
// transmit(). Nothing is freed while either can be inside it: see stepStop().
static std::atomic<esp_netif_t*> sNetif{nullptr};
static esp_netif_ip_info_t  sIpInfo;
static std::atomic<bool>    sHostOpened{false};   // the host set its packet filter: it opened the interface
static std::atomic<bool>    sEnabled{false};      // the operator's switch, as last applied
static bool                 sMounted = false;     // the device was mounted on the last poll
static std::atomic<bool>    sUp{false};           // the netif is started and connected; read on three tasks
// Whoever is inside the interface right now: tud_network_recv_cb, on the USB
// task. Counted for the same reason the ring's users are — the teardown nulls
// the handle and then waits for this to reach zero before destroying it.
static std::atomic<int>     sNetifUsers{0};
// The build/teardown policy both lazy links share (LazyStart.h).
static LocalLink::LazyStart sLazy;
static std::atomic<uint32_t> sRx{0}, sTx{0}, sTxDropped{0};

// Frames for the host wait here between the TCP/IP task and the USB task.
// The class driver keeps no lock: its transmit path runs on whichever task
// calls it and its completion path on the USB task, and the two walk the
// same NTB lists — so every call into it is made from the USB task, through
// usbd_defer_func(), which is how ESP-IDF's own glue does it. The ring also
// carries a burst: the driver holds one NTB, in flight for a millisecond or
// two per frame, while lwIP sends a window's worth of segments in one call;
// dropping what did not fit left TCP over the cable at one segment per ACK.
//
// The TCP/IP task fills, the USB task empties, and neither waits: a full
// ring drops the frame (TCP retransmits), and a frame that has waited
// kTxStaleMs is waiting on a host that has stopped draining and is dropped
// too. The radio and the transport must never wait on the USB cable.
namespace {
constexpr size_t   kTxSlots   = 4;
constexpr uint32_t kTxStaleMs = 200;
constexpr uint64_t kTxRetryUs = 1000;   // the NTB frees when a transfer completes, which the driver does not announce

struct TxSlot { uint16_t len; uint32_t queuedMs; uint8_t data[CFG_TUD_NET_MTU]; };
// Six kilobytes, and it used to be six kilobytes of .bss on every board that
// carries the composite device — paid whether usb0 was switched on or off,
// which is the thing this file's switch was supposed to decide. On the heap
// now, claimed when the link comes up and given back when it goes down.
std::atomic<TxSlot*>  sTxSlots{nullptr};
// Whoever is inside the ring right now, on the TCP/IP task (transmit) or the
// USB task (drain). The teardown nulls the pointer and then waits for this to
// reach zero, which is airtight in the one direction that matters: a user
// that took its reference before the null is counted here and waited for, and
// one that arrives after it reads null and leaves. Stopping the retry timer
// is not enough on its own — esp_timer_stop disarms but does not wait for a
// callback already dispatched, and that callback's whole job is to queue
// another drain.
std::atomic<int>      sRingUsers{0};
std::atomic<uint32_t> sTxHead{0};       // next slot to fill: the TCP/IP task's
std::atomic<uint32_t> sTxTail{0};       // next slot to send: the USB task's
esp_timer_handle_t    sTxRetry = nullptr;

// On the USB task: hands the ring to the class driver, as many frames as it
// will take, and comes back for the rest once the NTB has gone out.
void drain(void*) {
  sRingUsers.fetch_add(1, std::memory_order_acq_rel);
  TxSlot* slots = sTxSlots.load(std::memory_order_acquire);
  if (!slots) {                                    // the link went down while this was queued
    sRingUsers.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }
  while (sTxTail.load(std::memory_order_acquire) != sTxHead.load(std::memory_order_acquire)) {
    TxSlot& s = slots[sTxTail % kTxSlots];
    if (!sUp || (int32_t)(millis() - s.queuedMs) > (int32_t)kTxStaleMs) {
      sTxDropped++;
      sTxTail.fetch_add(1, std::memory_order_release);
      continue;
    }
    if (!tud_network_can_xmit(s.len)) {
      if (sTxRetry) esp_timer_start_once(sTxRetry, kTxRetryUs);   // already running: the same moment
      sRingUsers.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    tud_network_xmit(s.data, s.len);               // copied through xmit_cb, synchronously
    sTx++;
    sTxTail.fetch_add(1, std::memory_order_release);
  }
  sRingUsers.fetch_sub(1, std::memory_order_acq_rel);
}

void retryDrain(void*) { usbd_defer_func(drain, nullptr, false); }
}

// On the TCP/IP task, which carries the portal and the RNS transport too.
static esp_err_t transmit(void*, void* buffer, size_t len) {
  sRingUsers.fetch_add(1, std::memory_order_acq_rel);
  TxSlot* slots = sTxSlots.load(std::memory_order_acquire);
  if (!sUp || !slots || len > CFG_TUD_NET_MTU) {
    sRingUsers.fetch_sub(1, std::memory_order_acq_rel);
    return ESP_FAIL;
  }
  const uint32_t head = sTxHead.load(std::memory_order_relaxed);
  const bool waiting = head != sTxTail.load(std::memory_order_acquire);
  if (head - sTxTail.load(std::memory_order_acquire) >= kTxSlots) {
    sTxDropped++;
    sRingUsers.fetch_sub(1, std::memory_order_acq_rel);
    return ESP_FAIL;
  }
  TxSlot& s = slots[head % kTxSlots];
  memcpy(s.data, buffer, len);
  s.len = (uint16_t)len;
  s.queuedMs = millis();
  sTxHead.store(head + 1, std::memory_order_release);
  usbd_defer_func(drain, nullptr, false);
  // A kick can be lost when the USB task's queue is full; frames already
  // waiting get the timer as well, so nothing sits in the ring unannounced.
  if (waiting && sTxRetry) esp_timer_start_once(sTxRetry, kTxRetryUs);
  sRingUsers.fetch_sub(1, std::memory_order_acq_rel);
  return ESP_OK;
}

static void freeRxBuffer(void*, void* buffer) { free(buffer); }

struct Driver { esp_netif_driver_base_t base; };

static esp_err_t postAttach(esp_netif_t* netif, esp_netif_iodriver_handle h) {
  esp_netif_driver_ifconfig_t cfg = {};
  cfg.handle = h;
  cfg.transmit = transmit;
  cfg.transmit_wrap = nullptr;
  cfg.driver_free_rx_buffer = freeRxBuffer;
  static_cast<Driver*>(h)->base.netif = netif;
  return esp_netif_set_driver_config(netif, &cfg);
}

static Driver sDriver = { { postAttach, nullptr } };

// The 1200-baud touch, through the sequencer. The core would restart into
// the ROM by itself from inside the USB task the moment the host set that
// baud rate, skipping the quiesce and the detach above; with its own reboot
// disabled the line-coding event still arrives, and the request joins the
// queue like one from the console or the API. esptool's DTR/RTS pattern on
// this port, which the core's reboot also honoured, is not offered: the
// host tool never uses it here, since the downloader appears on another
// port.
// Noted here, completed in poll(): the USB event task that hears the
// line-coding change carries a stack too small for the sequencer's own
// logging — a request made from this context died inside vsnprintf, and the
// coredump of boot 906 named this exact handler. The loop task has the
// stack; the touch can afford its one poll pass of latency.
static std::atomic<uint32_t> sTouchAtMs{0};    // 0 = none; else millis of the touch

static void onLineCoding(void*, esp_event_base_t, int32_t, void* data) {
  const auto* d = static_cast<arduino_usb_cdc_event_data_t*>(data);
  if (!d || d->line_coding.bit_rate != 1200) return;
  const uint32_t now = millis();
  sTouchAtMs.store(now ? now : 1);
}

// What the device needs whether or not the network link is switched on: the
// 1200-baud touch has to work either way, and the retry timer is a handle
// small enough that taking it down and putting it back would be more risk
// than it is worth (a dispatched callback outliving its own timer). The
// interface, its DHCP server and the six-kilobyte transmit ring are the
// switch's, below.
void begin() {
  static bool begun = false;
  if (begun) return;
  begun = true;
  // The core's own reboot on this port is already off: setup() switches it
  // off beside Serial.begin(), so that no touch in the seconds before this
  // point restarts the chip behind the sequencer's back.
  Serial.onEvent(ARDUINO_USB_CDC_LINE_CODING_EVENT, onLineCoding);
  const esp_timer_create_args_t retry = {
    .callback = retryDrain, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
    .name = "usb0-tx", .skip_unhandled_events = true,
  };
  if (esp_timer_create(&retry, &sTxRetry) != ESP_OK) {
    sTxRetry = nullptr;
    log_e("usb0: the transmit retry timer could not be created; usb0 will not start");
  }
}

// Everything the switch decides. Built on the way on; given back on the way
// off, a step to a pass of the loop, because nothing here may be freed while
// the USB task or the TCP/IP task can still be inside it (stepStop, below).
// True when the link is up and whole. A false answer latches (LazyStart.h):
// poll() runs hundreds of times a second, and a node out of RAM must not bury
// the heap figures under its own complaint about them.
static bool startUsb() {
  if (sNetif) return true;
  // Without the retry timer a frame the class driver refuses is never
  // re-offered, and the link stalls until some later transmit happens to kick
  // the drain. begin() says so when it cannot create it; this is what used to
  // keep the interface from being built at all, and still does.
  if (!sTxRetry) {
    log_e("usb0: no retry timer, so the link would stall on the first busy frame; not starting");
    return false;
  }
  TxSlot* slots = (TxSlot*)heap_caps_malloc(sizeof(TxSlot) * kTxSlots,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!slots) {
    log_e("usb0: no room for the %u B transmit ring; the link stays down",
          (unsigned)(sizeof(TxSlot) * kTxSlots));
    return false;
  }
  sTxHead = sTxTail = 0;
  sIpInfo.ip.addr = sIpInfo.gw.addr = (uint32_t)address();
  sIpInfo.netmask.addr = htonl(kUsbNetmask);

  // The access point's shape — a DHCP server, up as soon as it is started —
  // on the Ethernet stack, under our own key and address.
  esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
  base.if_key = "USB_NCM_DEF";
  base.if_desc = "usb";
  base.route_prio = 20;
  base.ip_info = &sIpInfo;
  esp_netif_config_t cfg = { .base = &base, .driver = nullptr, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
  esp_netif_t* netif = esp_netif_new(&cfg);
  if (!netif) {
    log_e("usb0: the interface could not be created; the link stays down");
    heap_caps_free(slots);                 // nothing half-built is left behind
    return false;
  }
  esp_netif_set_mac(netif, sNodeMac);
  // A way to reach the node, not a way out: the offer carries no router, so
  // the host keeps its default route where it was and never sends the wider
  // world down a cable that goes nowhere. ESP-IDF's server names itself as
  // DNS whenever it is told to name nobody, so that option cannot be
  // dropped; the resolver behind it refuses every query that does not
  // arrive on the access point (CaptiveDns.h), which is what lets the
  // host's own resolver move on. The access point keeps the router, because
  // its captive portal needs it.
  // (OFFER_START is the empty set of offers; OFFER_END, despite the name,
  // is every bit set.)
  dhcps_offer_t none = OFFER_START;
  if (esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &none, sizeof(none)) != ESP_OK ||
      esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &none, sizeof(none)) != ESP_OK)
    log_w("usb0: could not strip router/DNS from the DHCP offer");
  // One address in the pool, and it is the one the documentation names as the
  // host's static fallback (LocalLinkState.h, usbHostAddress). Only one host
  // is ever on this cable, so a range buys nothing — and left to itself the
  // server hands out whatever IDF's default pool starts at, which is .2 today
  // and is not a promise anybody made. A host configured by hand and a host
  // that asked now land on the same address either way.
  const IPAddress host(htonl(usbHostAddress(sMacLastOctet)));
  dhcps_lease_t lease = {};
  lease.enable = true;
  lease.start_ip.addr = lease.end_ip.addr = htonl(usbHostAddress(sMacLastOctet));
  if (esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease)) != ESP_OK)
    log_w("usb0: could not pin the DHCP pool to %s", host.toString().c_str());
  esp_netif_attach(netif, &sDriver);
  // Published only once it is whole, and the ring before it: the first thing
  // that looks at either is a callback on another task.
  sTxSlots.store(slots, std::memory_order_release);
  sNetif.store(netif, std::memory_order_release);
  log_i("usb0: composite device %s, host MAC %s, node at %s (ring %u B)", USB_PRODUCT, sHostMacStr,
        address().toString().c_str(), (unsigned)(sizeof(TxSlot) * kTxSlots));
  return true;
}

// ---------------------------------------------------------------------------
// 3. The state machine
// ---------------------------------------------------------------------------
bool linkUp() { return sUp; }

// The interface goes down the way the access point's does: stopped, not
// disconnected first. esp_netif's stop returns early on an interface that is
// already down, leaving its DHCP server running; the access point's handlers
// call start and stop alone, and so does this.
static void bringDown() {
  sUp = false;                                      // recv_cb and transmit() check it first
  if (esp_netif_t* n = sNetif.load()) esp_netif_action_stop(n, nullptr, 0, nullptr);
}

void detach() {
  if (sUp) bringDown();
  tud_disconnect();
  // The soft disconnect alone leaves the PHY's pull-up on D+, and the core's
  // hand-over raises the serial-JTAG unit's pull-up on the same line within
  // microseconds of dropping this one. So the wire is made unambiguous:
  // pull-ups off and pull-downs on through the PHY's override, both units'
  // pads off the pins, and a moment for a hub to see it. A root port
  // notices the change with or without this. One bench hub did not notice
  // it either way — it reported the departure only when a transfer to the
  // vanished device failed, twenty seconds and more later, whatever the
  // device did — which is why the host tool waits rather than gives up
  // (device.py, _COMPOSITE_DOWNLOADER_S).
  const usb_wrap_pull_override_vals_t se0 = { .dp_pu = false, .dm_pu = false, .dp_pd = true, .dm_pd = true };
  usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &se0);
  usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
  usb_serial_jtag_ll_phy_enable_pad(false);
  delay(300);
}
bool hostOpened() { return sHostOpened; }
uint32_t rxPackets() { return sRx; }
uint32_t txPackets() { return sTx; }
uint32_t txDropped() { return sTxDropped; }

// Giving the interface and the ring back, a step to a pass of the loop. What
// makes it safe is not one barrier but two different kinds, because there are
// two different things that can still be holding what is about to be freed:
//
//   1. sUp = false and the handles nulled — transmit(), drain() and recv_cb
//      all read them and leave, so no new user can start.
//   2. Wait for the users already inside to leave (sRingUsers, sNetifUsers).
//      A count rather than a barrier, because esp_timer_stop only disarms the
//      retry timer: a callback already dispatched still runs, and its whole
//      job is to queue another drain, which a barrier posted before it would
//      not cover.
//   3. A no-op through lwIP's own mailbox. esp_netif_receive only *posts* the
//      frame — CONFIG_LWIP_TCPIP_CORE_LOCKING is on and CORE_LOCKING_INPUT is
//      not, so tcpip_input queues and returns, and esp_netif_action_stop runs
//      inline on this task under the core lock and is therefore no barrier at
//      all for what is already in that queue. tcpip_callback goes through the
//      same mailbox, so when it runs, every frame posted before it has been
//      consumed. Destroying the netif without this leaves the tcpip thread to
//      dequeue a pbuf and run it against freed memory.
//   4. Only then: stop, destroy, free.
//
// If a step never completes the machine stays in it. The memory stays
// claimed, usb0 stays down and the warning is said once — not freeing is
// harmless, and freeing under a live callback is not. It deliberately does
// not give up and re-arm: that would re-post the barriers every few seconds
// for as long as the switch stayed off, and a give-up that left the handles
// nulled would let the next enable build a second interface over the first.
enum class Stop : uint8_t { Idle = 0, Quiesce, Lwip, Finish };
static Stop                 sStop = Stop::Idle;          // loop task only
static uint32_t             sStopAskedMs = 0;
static bool                 sStopWarned = false;
static esp_netif_t*         sStopNetif = nullptr;        // held while the steps run
static TxSlot*              sStopSlots = nullptr;
static constexpr uint32_t   kStepWaitMs = 5000;

// Generation-stamped, so a no-op left over from an earlier teardown cannot
// satisfy a later one: what matters is that a message posted *after* the
// frames has been through the queue, and only the current generation's has.
static std::atomic<uint32_t> sLwipAsked{0};
static std::atomic<uint32_t> sLwipSeen{0};
static void lwipBarrier(void*) { sLwipSeen.store(sLwipAsked.load(std::memory_order_acquire),
                                                 std::memory_order_release); }

static bool stalled(uint32_t nowMs, const char* what) {
  if (nowMs - sStopAskedMs <= kStepWaitMs) return false;
  if (!sStopWarned) {
    sStopWarned = true;
    log_w("usb0: %s has not finished; the interface is kept rather than freed under it", what);
  }
  return true;
}

static void stepStop(uint32_t nowMs) {
  switch (sStop) {
    case Stop::Idle:
      return;

    case Stop::Quiesce:
      // Everyone who took a reference before the handles were nulled.
      if (sRingUsers.load(std::memory_order_acquire) > 0 ||
          sNetifUsers.load(std::memory_order_acquire) > 0) {
        stalled(nowMs, "a driver callback");
        return;
      }
      sLwipAsked.fetch_add(1, std::memory_order_acq_rel);
      if (tcpip_callback(lwipBarrier, nullptr) != ERR_OK) {
        // The mailbox is full. Nothing is freed on a maybe; try again next
        // pass, which is what leaving the state alone does.
        stalled(nowMs, "lwIP's mailbox");
        return;
      }
      sStop = Stop::Lwip;
      return;

    case Stop::Lwip:
      if (sLwipSeen.load(std::memory_order_acquire) != sLwipAsked.load(std::memory_order_acquire)) {
        stalled(nowMs, "the lwIP queue");
        return;
      }
      sStop = Stop::Finish;
      return;

    case Stop::Finish:
      if (sStopNetif) {
        esp_netif_action_stop(sStopNetif, nullptr, 0, nullptr);
        esp_netif_destroy(sStopNetif);
        sStopNetif = nullptr;
      }
      if (sStopSlots) { heap_caps_free(sStopSlots); sStopSlots = nullptr; }
      sStop = Stop::Idle;
      sStopWarned = false;
      log_i("usb0: switched off; the interface and its %u B ring are given back",
            (unsigned)(sizeof(TxSlot) * kTxSlots));
      return;
  }
}

void poll(bool enabled) {
  const uint32_t now = millis();
  // The 1200-baud touch, before anything else and regardless of the link
  // switch — begin() promises the touch works either way. Requested from
  // here rather than the event handler that heard it (see onLineCoding).
  // A touch is honored only while the tool is plausibly still waiting: a
  // flag latched during a wedged loop must not reboot the node into the
  // downloader minutes later with nobody flashing.
  const uint32_t touchAt = sTouchAtMs.exchange(0);
  if (touchAt && now - touchAt < 3000) {
    const char* why = nullptr;
    const Bootloader::Refusal r = Bootloader::request(Bootloader::Target::Bootloader,
                                                      Bootloader::Source::Touch, 0, &why);
    // A refusal has nowhere to go but the log: the core posts the event only
    // when the coding changes, so the host tool opens at another speed
    // before touching again (device.py, touch_1200).
    if (r != Bootloader::Refusal::None) log_w("touch: refused (%s)", why ? why : "");
  }
  // The switch decides whether the interface exists at all, not merely
  // whether it answers (UsbNcm.h). A teardown once begun runs to its end
  // before anything is built again, and a build that failed is not retried
  // every pass (LazyStart.h).
  const bool busy = sStop != Stop::Idle;
  if (busy) {
    stepStop(now);
    return;
  }
  const bool built = sNetif.load(std::memory_order_acquire) != nullptr;
  sLazy.idle(enabled, built, busy);
  if (sLazy.shouldStart(enabled, built, busy)) {
    sLazy.built(startUsb());
    return;
  }
  if (sLazy.shouldStop(enabled, built, busy)) {
    // The host is told the carrier has gone before anything is taken apart.
    sEnabled = false;
    if (tud_mounted()) usbd_defer_func([](void*) { if (tud_mounted()) tud_network_link_state(0, false); }, nullptr, false);
    if (sUp) bringDown();
    if (sTxRetry) esp_timer_stop(sTxRetry);
    // Frames still between the indices go nowhere, and an operator reading
    // ncm_tx_dropped should see what the switch cost rather than a counter
    // that quietly skipped them.
    const uint32_t queued = sTxHead.load(std::memory_order_acquire) - sTxTail.load(std::memory_order_acquire);
    if (queued) sTxDropped += queued;
    // The host's side of the link goes with the interface; leaving this set
    // reports host_opened=yes for something that no longer exists.
    sHostOpened = false;
    sMounted = false;
    // Nulled here, before anything waits: from now on every callback reads
    // them and leaves, so the count below can only fall.
    sStopNetif = sNetif.exchange(nullptr, std::memory_order_acq_rel);
    sStopSlots = sTxSlots.exchange(nullptr, std::memory_order_acq_rel);
    sStopAskedMs = now;
    sStopWarned = false;
    sStop = Stop::Quiesce;
    stepStop(now);                               // a pass is not wasted waiting to begin
    return;
  }
  if (!sNetif) return;
  // Carrier is the cable: a mounted, awake device. The NCM class driver
  // gives no callback for the host opening its data interface — the
  // init_cb in TinyUSB's header is ECM/RNDIS's — so the interface is up
  // and serving DHCP from the moment the host has enumerated the device,
  // and the first frame decides the rest. The packet-filter request Linux
  // makes when it opens the interface is kept as a diagnostic.
  const bool mounted = tud_mounted() && !tud_suspended();
  if (!mounted) sHostOpened = false;
  // Tell the host the carrier state — when the operator's switch moves, and
  // again whenever the device has been mounted afresh. The class driver is
  // reset on every bus reset and takes its link state from its own default,
  // not from what we last sent: after a replug or a resume, a link this node
  // still considers up was never announced to the host that just arrived, and
  // its interface sits at NO-CARRIER until somebody toggles the switch. Sent
  // from the USB task, like every other call into the class driver.
  const bool remounted = mounted && !sMounted;
  if (enabled != sEnabled || remounted) {
    sEnabled = enabled;
    if (mounted) usbd_defer_func([](void*) { if (tud_mounted()) tud_network_link_state(0, sEnabled); }, nullptr, false);
  }
  sMounted = mounted;
  const bool want = enabled && mounted;
  if (want && !sUp) {
    esp_netif_action_start(sNetif, nullptr, 0, nullptr);
    esp_netif_action_connected(sNetif, nullptr, 0, nullptr);
    sUp = true;
    log_i("usb0: up, %s/24, DHCP serving the host", address().toString().c_str());
  } else if (!want && sUp) {
    bringDown();
    log_i("usb0: down (%s)", !enabled ? "switched off" : "host gone");
  }
}

} // namespace UsbNcm

// ---------------------------------------------------------------------------
// TinyUSB's class callbacks, on the USB task. They record and copy; the
// netif transitions happen in poll().
// ---------------------------------------------------------------------------
extern "C" {

// No tud_network_init_cb and no tud_network_mac_address here: the header
// declares both for ECM/RNDIS, and this NCM driver calls neither. The host
// learns its MAC from the descriptor string above.
void tud_network_set_packet_filter_cb(uint16_t) {
  UsbNcm::sHostOpened = true;
}

bool tud_network_default_link_state_cb(void) {
  return UsbNcm::sEnabled;
}

bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
  using namespace UsbNcm;
  if (sUp && size) {
    // Internal RAM: a frame lives microseconds, and on a PSRAM board plain
    // malloc() would put this copy where lwIP then reads it back through
    // the cache, twice.
    sNetifUsers.fetch_add(1, std::memory_order_acq_rel);
    esp_netif_t* netif = sNetif.load(std::memory_order_acquire);
    void* copy = netif ? heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
    if (copy) {
      memcpy(copy, src, size);
      // From here the buffer belongs to the stack, which frees it through
      // freeRxBuffer once the frame has been consumed — on every path,
      // including the ones where it refuses the frame. Freeing it here as
      // well on a non-OK return would be a second free of the same block;
      // this build cannot even see one, since esp_netif_receive reports no
      // errors unless CONFIG_ESP_NETIF_RECEIVE_REPORT_ERRORS is set.
      esp_netif_receive(netif, copy, size, nullptr);
      sRx++;
    }
    sNetifUsers.fetch_sub(1, std::memory_order_acq_rel);
  }
  tud_network_recv_renew();                         // the class buffer is ours no longer
  return true;
}

uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg) {
  memcpy(dst, ref, arg);
  return arg;
}

} // extern "C"
#endif
