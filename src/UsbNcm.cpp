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
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_defaults.h>
#include <dhcpserver/dhcpserver.h>
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include <hal/usb_wrap_ll.h>
#include <hal/usb_serial_jtag_ll.h>
#include <soc/usb_wrap_struct.h>
#include "LocalLinkState.h"
#include "Bootloader.h"

using LocalLink::usbNodeAddress;
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

static void octets(uint32_t hostOrder, uint8_t out[4]) {
  out[0] = hostOrder >> 24; out[1] = hostOrder >> 16; out[2] = hostOrder >> 8; out[3] = hostOrder;
}

IPAddress address() {
  uint8_t o[4]; octets(usbNodeAddress(sMacLastOctet), o);
  return IPAddress(o[0], o[1], o[2], o[3]);
}

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
  if (!epNotify || !epIn || !epOut) return 0;
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
static esp_netif_t*         sNetif = nullptr;
static esp_netif_ip_info_t  sIpInfo;
static std::atomic<bool>    sHostOpened{false};   // the host set its packet filter: it opened the interface
static std::atomic<bool>    sEnabled{false};      // the operator's switch, as last applied
static std::atomic<bool>    sUp{false};           // the netif is started and connected; read on three tasks
static std::atomic<uint32_t> sRx{0}, sTx{0}, sTxDropped{0};

// TinyUSB's transmit is a copy through xmit_cb, synchronous, so the frame
// lwIP hands over is consumed before transmit() returns and lwIP may free it.
static esp_err_t transmit(void*, void* buffer, size_t len) {
  if (!sUp || len > CFG_TUD_NET_MTU) return ESP_FAIL;
  // This runs on the TCP/IP task, which carries the portal and the RNS
  // transport too. The class driver holds one NTB in flight; if it is busy
  // the frame is dropped here and now — TCP retransmits, and the radio and
  // the transport must never wait on the USB cable, least of all on a host
  // that has stopped draining it.
  if (!tud_network_can_xmit((uint16_t)len)) { sTxDropped++; return ESP_FAIL; }
  tud_network_xmit(buffer, (uint16_t)len);
  sTx++;
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
static void onLineCoding(void*, esp_event_base_t, int32_t, void* data) {
  const auto* d = static_cast<arduino_usb_cdc_event_data_t*>(data);
  if (!d || d->line_coding.bit_rate != 1200) return;
  const char* why = nullptr;
  const Bootloader::Refusal r = Bootloader::request(Bootloader::Target::Bootloader, Bootloader::Source::Touch, 0, &why);
  // A refusal has nowhere to go but the log: the core posts this event only
  // when the coding changes, so the host tool opens at another speed before
  // touching again (device.py, touch_1200).
  if (r != Bootloader::Refusal::None) log_w("touch: refused (%s)", why ? why : "");
}

void begin() {
  if (sNetif) return;
  Serial.enableReboot(false);
  Serial.onEvent(ARDUINO_USB_CDC_LINE_CODING_EVENT, onLineCoding);
  uint8_t o[4];
  octets(usbNodeAddress(sMacLastOctet), o);
  esp_netif_set_ip4_addr(&sIpInfo.ip, o[0], o[1], o[2], o[3]);
  esp_netif_set_ip4_addr(&sIpInfo.gw, o[0], o[1], o[2], o[3]);
  octets(kUsbNetmask, o);
  esp_netif_set_ip4_addr(&sIpInfo.netmask, o[0], o[1], o[2], o[3]);

  // The access point's shape — a DHCP server, up as soon as it is started —
  // on the Ethernet stack, under our own key and address.
  esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
  base.if_key = "USB_NCM_DEF";
  base.if_desc = "usb";
  base.route_prio = 20;
  base.ip_info = &sIpInfo;
  esp_netif_config_t cfg = { .base = &base, .driver = nullptr, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
  sNetif = esp_netif_new(&cfg);
  if (!sNetif) { log_e("usb0: esp_netif_new failed"); return; }
  esp_netif_set_mac(sNetif, sNodeMac);
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
  if (esp_netif_dhcps_option(sNetif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &none, sizeof(none)) != ESP_OK ||
      esp_netif_dhcps_option(sNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &none, sizeof(none)) != ESP_OK)
    log_w("usb0: could not strip router/DNS from the DHCP offer");
  esp_netif_attach(sNetif, &sDriver);
  log_i("usb0: composite device %s, host MAC %s, node at %s", USB_PRODUCT, sHostMacStr, address().toString().c_str());
}

// ---------------------------------------------------------------------------
// 3. The state machine
// ---------------------------------------------------------------------------
bool linkUp() { return sUp; }

void detach() {
  if (sUp) {
    sUp = false;                                    // before the stop, as in poll()
    esp_netif_action_disconnected(sNetif, nullptr, 0, nullptr);
    esp_netif_action_stop(sNetif, nullptr, 0, nullptr);
  }
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

void poll(bool enabled) {
  if (!sNetif) return;
  // Carrier is the cable: a mounted, awake device. The NCM class driver
  // gives no callback for the host opening its data interface — the
  // init_cb in TinyUSB's header is ECM/RNDIS's — so the interface is up
  // and serving DHCP from the moment the host has enumerated the device,
  // and the first frame decides the rest. The packet-filter request Linux
  // makes when it opens the interface is kept as a diagnostic.
  const bool mounted = tud_mounted() && !tud_suspended();
  if (!mounted) sHostOpened = false;
  if (enabled != sEnabled) {
    sEnabled = enabled;
    // Tell the host, so its interface shows the carrier the operator set.
    if (mounted) tud_network_link_state(0, enabled);
  }
  const bool want = enabled && mounted;
  if (want && !sUp) {
    esp_netif_action_start(sNetif, nullptr, 0, nullptr);
    esp_netif_action_connected(sNetif, nullptr, 0, nullptr);
    sUp = true;
    log_i("usb0: up, %s/24, DHCP serving the host", address().toString().c_str());
  } else if (!want && sUp) {
    // Down first, then stop: a frame arriving on the USB task between the
    // two must not be handed to an interface that has just been torn down.
    sUp = false;
    esp_netif_action_disconnected(sNetif, nullptr, 0, nullptr);
    esp_netif_action_stop(sNetif, nullptr, 0, nullptr);
    log_i("usb0: down (%s)", !enabled ? "switched off" : "host gone");
  }
}

} // namespace UsbNcm

// ---------------------------------------------------------------------------
// TinyUSB's class callbacks, on the USB task. They record and copy; the
// netif transitions happen in poll().
// ---------------------------------------------------------------------------
extern "C" {

// Referenced by TinyUSB's network classes; the descriptor string above is
// what the host actually reads.
uint8_t tud_network_mac_address[6];

void tud_network_init_cb(void) {
  memcpy(tud_network_mac_address, UsbNcm::sHostMac, 6);
}

void tud_network_set_packet_filter_cb(uint16_t) {
  UsbNcm::sHostOpened = true;
}

bool tud_network_default_link_state_cb(void) {
  return UsbNcm::sEnabled;
}

bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
  using namespace UsbNcm;
  if (sUp && size) {
    void* copy = malloc(size);
    if (copy) {
      memcpy(copy, src, size);
      // From here the buffer belongs to the stack, which frees it through
      // freeRxBuffer once the frame has been consumed.
      if (esp_netif_receive(sNetif, copy, size, nullptr) == ESP_OK) sRx++;
    }
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
