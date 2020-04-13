//
//  IWLCachedScan.cpp
//  AppleIntelWifiAdapter
//
//  Created by Harrison Ford on 3/31/20.
//  Copyright © 2020 IntelWifi for MacOS authors. All rights reserved.
//

#include "IWLCachedScan.hpp"

#include <sys/kpi_mbuf.h>
#include <sys/mbuf.h>

#include "IWLDebug.h"
#include "apple80211/apple80211_ioctl.h"
#include "compat/openbsd/sys/endian.h"
#include "fw/api/rs.h"

#define super OSObject
OSDefineMetaClassAndStructors(IWLCachedScan, OSObject);

#define check_packet()                                              \
  if (!packet) {                                                    \
    IWL_ERR(0, "Packet should exist in IWLCachedScan (func: %s)\n", \
            __FUNCTION__);                                          \
    return NULL;                                                    \
  }

/*
    The idea is for this class to be as non-copy as possible.
    The point of fixed offsets are to do such
    (but, they are indeed safe as these are fixed offsets defined within the
   IEEE 802.11 spec)
*/

SInt32 orderCachedScans(const OSMetaClassBase* obj1,
                        const OSMetaClassBase* obj2, void* context) {
  IWLCachedScan* cachedScan_l = OSDynamicCast(IWLCachedScan, obj1);
  if (!cachedScan_l) return 0;

  IWLCachedScan* cachedScan_r = OSDynamicCast(IWLCachedScan, obj2);
  if (!cachedScan_r) return 0;

  if (cachedScan_l->getTimestamp() <
      cachedScan_r->getTimestamp())  // smaller timestamps go before
    return 1;
  else if (cachedScan_l->getTimestamp() == cachedScan_r->getTimestamp())
    return 0;
  else  // object 2 is older, put it before
    return -1;
}

bool IWLCachedScan::update(iwl_rx_phy_info* phy_info, int rssi, int noise) {
  this->noise = noise;
  this->rssi = rssi;

  memcpy(&this->phy_info, phy_info, sizeof(iwl_rx_phy_info));  // necessary

  if (le16toh(phy_info->channel) != this->channel.channel) {
    // it changed channels on us!
    this->channel.channel = le16toh(phy_info->channel);
  }
  this->absolute_time = mach_absolute_time();

  return true;
}

uint8_t* search_ie(uint8_t* ie, uint16_t size, uint8_t capa) {
  size_t ie_len = size;
  uint8_t* ie_iterator = ie;
  while (1) {
    uint8_t v6;
    if (ie_len < 3 || *ie_iterator != capa) {
      uint8_t sect_len = *(ie_iterator + 1);
      ie_len = ie_len - 2 - sect_len;
      if (ie_len <= 0) return NULL;

      ie_iterator = (ie_iterator + sect_len + 2);
      v6 = *(ie_iterator + 1);
      goto LABEL_10;
    }
    v6 = *(ie_iterator + 1);
    if (ie_len - 2 >= v6) {
      return ie_iterator;
    }
  LABEL_10:
    if (v6 + 2 > ie_len) return NULL;
  }
}

bool IWLCachedScan::init(mbuf_t mbuf, int offset, int whOffset,
                         iwl_rx_phy_info* phy_info, int rssi, int noise) {
  if (!super::init()) return false;

  errno_t err = mbuf_dup(mbuf, MBUF_DONTWAIT, &buf);

  if (err != 0) {
    IWL_ERR(0, "mbuf dup complained\n");
    return false;
  }

  if (!buf) return false;

  packet = reinterpret_cast<iwl_rx_packet*>(
      (u8*)mbuf_data((mbuf_t)buf) + (offset));  // NOLINT(readability/casting)

  if (!packet) return false;

  wh = reinterpret_cast<ieee80211_frame*>(packet->data + whOffset);
  iwl_rx_mpdu_res_start* rx_res =
      reinterpret_cast<iwl_rx_mpdu_res_start*>(packet->data);

  this->ie_len = rx_res->byte_count - 36;

  if (this->ie_len <= 0) return false;

  if (le16toh(rx_res->byte_count) <= 36) return false;

  this->ie = (reinterpret_cast<uint8_t*>(wh) + 36);

  this->noise = noise;
  this->rssi = rssi;

  memcpy(&this->phy_info, phy_info, sizeof(iwl_rx_phy_info));  // necessary
  this->absolute_time = mach_absolute_time();

  if (this->ie[0] != 0x00) {
    IWL_ERR(0, "potentially uncompliant frame\n");
    IWL_INFO(0, "wh: %x, ie: %x\n", reinterpret_cast<uint8_t*>(wh)[39], ie[5]);
    IWL_INFO(0, "wh: %x, ie: %x\n", reinterpret_cast<uint8_t*>(wh)[38],
             ie[4]);  // first byte
    IWL_INFO(0, "wh: %x, ie: %x\n", reinterpret_cast<uint8_t*>(wh)[37],
             ie[3]);  // len
    IWL_INFO(0, "wh: %x, ie: %x\n", reinterpret_cast<uint8_t*>(wh)[36],
             ie[2]);  // indicator
    IWL_INFO(0, "wh: %x, ie: %x\n", reinterpret_cast<uint8_t*>(wh)[35], ie[1]);
    IWL_INFO(0, "wh: %x, ie: %x\n", reinterpret_cast<uint8_t*>(wh)[34], ie[0]);
    IWL_INFO(0, "wh: %x\n", reinterpret_cast<uint8_t*>(wh)[34]);
    IWL_INFO(0, "wh: %x\n", reinterpret_cast<uint8_t*>(wh)[33]);
    IWL_INFO(0, "wh: %x\n", reinterpret_cast<uint8_t*>(wh)[32]);
    IWL_INFO(0, "wh: %x\n", reinterpret_cast<uint8_t*>(wh)[31]);
    IWL_INFO(0, "wh: %x\n", reinterpret_cast<uint8_t*>(wh)[30]);
    IWL_INFO(0, "wh: %x\n", reinterpret_cast<uint8_t*>(wh)[29]);
    return false;
  }

  this->n_basic_rates = 0;
  this->n_ext_rates = 0;
  this->n_rates = 0;
  this->n_hw_rates = 0;

  uint8_t* rate_ptr = search_ie(this->ie, this->ie_len, 0x01);
  uint8_t* ext_rates = search_ie(this->ie, this->ie_len, 0x32);

  size_t index = 0;

  if (rate_ptr == NULL) return NULL;

  uint8_t rate_size = *(rate_ptr + 1);
  if (rate_size != 8) {
    IWL_ERR(0, "rate set is NOT correct\n");
  } else {
    for (int i = 0; i < 8; i++) {
      uint8_t rate = (*(rate_ptr + 2 + i));
      if (rate > 0x80) {
        this->basic_rates[this->n_basic_rates++] = rate;
      } else {
        this->rates[this->n_rates++] = rate;
      }

      this->hw_rates[this->n_hw_rates++] = ((rate & 0x3F) / 2) * 10;
    }

    if (ext_rates != NULL) {
      uint8_t n_ext_rates = *(ext_rates + 1);
      if (n_ext_rates > 4) n_ext_rates = 4;

      this->n_rates += n_ext_rates;

      for (int i = 0; i < 4; i++) {
        this->ext_rates[i] = (*(ext_rates + 2 + i));
      }
    }
  }

  channel.version = APPLE80211_VERSION;
  channel.channel = le16toh(this->phy_info.channel);

  if (channel.channel < 15)
    channel.flags |= APPLE80211_C_FLAG_2GHZ;
  else
    channel.flags |= APPLE80211_C_FLAG_5GHZ;

  // 20 - 40 is valid for HT / VHT, but 80 - 160 is ONLY valid for VHT

  uint8_t* vht_op = search_ie(this->ie, this->ie_len, 0xC0);
  if (vht_op && channel.channel > 15) {
    this->vht_supported = true;

    uint8_t vht_width = *(vht_op + 2);

    if (vht_width) {
      channel.flags |= APPLE80211_C_FLAG_80MHZ;
    } else if (vht_width == 0x00) {
      channel.flags |= APPLE80211_C_FLAG_40MHZ;
    }
  }

  uint8_t* ht_op = search_ie(this->ie, this->ie_len, 0x3D);
  if (ht_op) {
    this->ht_supported = true;

    if (!this->vht_supported) {
      uint8_t ht_width = *(ht_op + 3);

      if (1 &
          (ht_width >>
           2)) {  // bit position 3 signals whether or not any width is allowed
        channel.flags |= APPLE80211_C_FLAG_40MHZ;
      } else {
        channel.flags |= APPLE80211_C_FLAG_20MHZ;
      }
    }
  }

  if (!this->vht_supported && !this->ht_supported) {
    channel.flags |= APPLE80211_C_FLAG_20MHZ;
  }

  return true;
}

void IWLCachedScan::free() {
  super::free();

  if (buf) {
    mbuf_free(buf);
  }
  packet = NULL;
}

apple80211_channel IWLCachedScan::getChannel() { return channel; }

uint64_t IWLCachedScan::getTimestamp() { return phy_info.timestamp; }

uint64_t IWLCachedScan::getSysTimestamp() { return absolute_time; }

const char* IWLCachedScan::getSSID() {  // ensure to free this resulting buffer
  check_packet()

      if (ie[0] != 0x00) {
    IWL_ERR(0, "haven't handled this yet\n");
    return NULL;
  }

  uint8_t ssid_len = getSSIDLen();

  const char* ssid =
      (const char*)kzalloc(ssid_len + 1);  // include null terminator

  if (!ssid) return NULL;

  memcpy((void*)ssid, &ie[2],  // NOLINT(readability/casting)
         ssid_len);  // 0x00 == type, 0x01 == size, 0x02 onwards == data

  return ssid;
}

uint32_t IWLCachedScan::getSSIDLen() {
  check_packet()

      if (ie[0] != 0x00) {
    IWL_ERR(0, "haven't handled this yet\n");
    return NULL;
  }

  uint8_t ssid_len = ie[1];

  if (ssid_len > 32) {
    IWL_ERR(0, "ssid length too large, clamping to 32\n");
    ssid_len = 32;
  }

  return ssid_len;
}

uint32_t IWLCachedScan::getRSSI() { return rssi; }

uint32_t IWLCachedScan::getNoise() { return noise; }

uint16_t IWLCachedScan::getCapabilities() {
  check_packet()

          return (*(reinterpret_cast<uint8_t*>(wh) + 35) << 8) |
      (*(reinterpret_cast<uint8_t*>(wh) + 34));
  // these are stored in the fixed parameters, offsets are fine here
}

uint8_t* IWLCachedScan::getBSSID() {
  check_packet()

      return &wh->i_addr3[0];
}

uint8_t IWLCachedScan::getNumRates() { return this->n_rates; }

uint8_t IWLCachedScan::getNumExtRates() { return this->n_ext_rates; }

uint8_t IWLCachedScan::getNumBasicRates() { return this->n_basic_rates; }

uint8_t IWLCachedScan::getNumHWRates() { return this->n_hw_rates; }

uint8_t* IWLCachedScan::getRates() {
  check_packet()

      return this->rates;
}

uint8_t* IWLCachedScan::getExtRates() {
  check_packet()

      return this->ext_rates;
}

uint8_t* IWLCachedScan::getBasicRates() {
  check_packet()

      return this->basic_rates;
}

uint8_t* IWLCachedScan::getHWRates() {
  check_packet()

      return this->hw_rates;
}

void* IWLCachedScan::getIE() {
  check_packet()

      return (void*)ie;  // NOLINT(readability/casting)
}

uint32_t IWLCachedScan::getIELen() { return ie_len; }

apple80211_scan_result*
IWLCachedScan::getNativeType() {  // be sure to free this too
  check_packet()

      result = reinterpret_cast<apple80211_scan_result*>(
          kzalloc(sizeof(apple80211_scan_result)));

  if (result == NULL) return NULL;

  result->version = APPLE80211_VERSION;

  // uint64_t nanosecs;
  // absolutetime_to_nanoseconds(this->getSysTimestamp(), &nanosecs);
  // result->asr_age = (nanosecs * (__int128)0x431BDE82D7B634DBuLL >> 64) >> 18;
  // // MAGIC compiler division..
  // I don't get this

  // result->asr_age = le32toh(this->phy_info.system_timestamp);
  result->asr_ie_len = this->getIELen();

  IWL_INFO(0, "IE length: %d\n", result->asr_ie_len);
  if (result->asr_ie_len != 0) {
    result->asr_ie_data = ie;

    // uint8_t* buf = (uint8_t*)result->asr_ie_data;
    // size_t out_sz = 0;
    // uint8_t* encode = base64_encode(buf, (size_t)result->asr_ie_len,
    // &out_sz); IWL_INFO(0, "IE: %s", encode);
  }

  result->asr_beacon_int = 100;

  uint8_t* rates = this->getRates();

  if (rates != NULL) {
    for (int i = 0; i < 4; i++) {
      result->asr_rates[i] = (this->rates[i] >> 1) & 0x3F;
    }

    for (int i = 0; i < 4; i++) {
      result->asr_rates[i + 4] = (this->basic_rates[i] >> 1) & 0x3F;
    }

    if (this->n_rates != 8 && this->n_rates == 12) {
      for (int i = 0; i < 4; i++) {
        result->asr_rates[i + 8] = (this->ext_rates[i] >> 1) & 0x3F;
      }
    }
    result->asr_nrates = this->getNumRates();
  }

  result->asr_cap = this->getCapabilities();

  result->asr_channel.channel = this->getChannel().channel;
  result->asr_channel.flags = this->getChannel().flags;
  result->asr_channel.version = 1;

  result->asr_noise = this->getNoise();
  result->asr_rssi = this->getRSSI();

  memcpy(&result->asr_bssid, this->getBSSID(), 6);

  result->asr_ssid_len = this->getSSIDLen();

  if (this->getSSIDLen() != 0) {
    const char* ssid = this->getSSID();

    if (ssid) {
      memcpy(&result->asr_ssid, ssid, this->getSSIDLen() + 1);
      // clang-format off
      IOFree((void*)ssid, this->getSSIDLen() + 1);  // NOLINT(readability/casting)
      // clang-format on
    }
  }

  // result->asr_age = le32toh(this->phy_info.system_timestamp);
  return result;
}

mbuf_t IWLCachedScan::getMbuf() { return buf; }
