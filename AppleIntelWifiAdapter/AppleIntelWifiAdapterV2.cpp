//
//  AppleIntelWifiAdapterV2.cpp
//  AppleIntelWifiAdapter
//
//  Created by Harrison Ford on 4/4/20.
//  Copyright © 2020 IntelWifi for MacOS authors. All rights reserved.
//

#include "AppleIntelWifiAdapterV2.hpp"

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/network/IONetworkMedium.h>

#include "IO80211Interface.h"
#include "IWLApple80211.hpp"
#include "IWLDebug.h"

OSDefineMetaClassAndStructors(AppleIntelWifiAdapterV2, IO80211Controller)
#define super IO80211Controller

#define MBit 1000000

    void AppleIntelWifiAdapterV2::releaseAll() {
  IWL_DEBUG(0, "Releasing everything\n");
  if (fInterrupt) {
    irqLoop->removeEventSource(fInterrupt);
    fInterrupt->disable();
    fInterrupt = NULL;
  }

  if (gate) {
    gate->release();
    gate = NULL;
  }
  if (irqLoop) {
    irqLoop->release();
    irqLoop = NULL;
  }

  if (workLoop) {
    workLoop->release();
    workLoop = NULL;
  }
  if (netif) {
    netif->release();
    netif = NULL;
  }

  if (drv) {
    drv->release();
    drv = NULL;
  }
}

void AppleIntelWifiAdapterV2::free() {
  IWL_DEBUG(0, "Driver free()\n");
  releaseAll();
  super::free();
}

bool AppleIntelWifiAdapterV2::init(OSDictionary *properties) {
  IWL_DEBUG(0, "Driver init()\n");
  if (!super::init(properties)) return false;

  return true;
}

IO80211Interface *AppleIntelWifiAdapterV2::getInterface() { return netif; }

IOService *AppleIntelWifiAdapterV2::probe(IOService *provider, SInt32 *score) {
  IWL_DEBUG(0, "Driver Probe()\n");
  if (!super::probe(provider, score)) return NULL;

  IOPCIDevice *pciDevice = OSDynamicCast(IOPCIDevice, provider);
  if (!pciDevice) {
    IWL_CRIT(0, "Not pci device");
    return NULL;
  }
  UInt16 vendorID = pciDevice->configRead16(kIOPCIConfigVendorID);
  UInt16 deviceID = pciDevice->configRead16(kIOPCIConfigDeviceID);
  UInt16 subSystemVendorID =
      pciDevice->configRead16(kIOPCIConfigSubSystemVendorID);
  UInt16 subSystemDeviceID = pciDevice->configRead16(kIOPCIConfigSubSystemID);
  UInt8 revision = pciDevice->configRead8(kIOPCIConfigRevisionID);

  bool valid = false;
  for (int i = 0; i < ARRAY_SIZE(iwl_hw_card_ids); i++) {
    pci_device_id dev = iwl_hw_card_ids[i];
    if (dev.device == deviceID) {
      valid = true;  // try to find one card..
      break;
    }
  }

  if (!valid) return NULL;

  IWL_DEBUG(
      0,
      "found pci device====>vendorID=0x%04x, deviceID=0x%04x, "
      "subSystemVendorID=0x%04x, subSystemDeviceID=0x%04x, revision=0x%02x\n",
      vendorID, deviceID, subSystemVendorID, subSystemDeviceID, revision);

  this->drv = new IWLMvmDriver();
  this->drv->m_pDevice = new IWLDevice();
  this->drv->m_pDevice->pciDevice = pciDevice;
  this->drv->m_pDevice->state = APPLE80211_S_INIT;
  IWL_DEBUG(0, "drv: %x, m_pDevice: %x\n", this->drv, this->drv->m_pDevice);

  return this;
}

bool AppleIntelWifiAdapterV2::createWorkLoop() {
  if (!workLoop) workLoop = IO80211WorkLoop::workLoop();

  return (workLoop != NULL);
}

IOWorkLoop *AppleIntelWifiAdapterV2::getWorkLoop() const { return workLoop; }

bool AppleIntelWifiAdapterV2::start(IOService *provider) {
  IWL_DEBUG(0, "Driver Start()\n");
  if (!super::start(provider)) return false;

  if (!this->drv) {
    IWL_CRIT(0, "Missing this->drv\n");
    releaseAll();
    return false;
  }

  this->drv->controller = static_cast<IO80211Controller *>(this);

  if (!this->drv->init()) return false;

  if (!this->drv->probe()) return false;

  irqLoop = IO80211WorkLoop::workLoop();

  initTimeout(irqLoop);

  if (!this->drv->m_pDevice) {
    IWL_CRIT(0, "Missing this->m_pDevice\n");
    releaseAll();
    return false;
  }

  if (!this->drv->m_pDevice->pciDevice) {
    IWL_CRIT(0, "Missing this->m_pDevice->pciDevice\n");
    releaseAll();
    return false;
  }

  gate = IOCommandGate::commandGate(this);
  if (!gate) {
    IWL_CRIT(0, "Failed to create command gate\n");
    releaseAll();
    return false;
  }

  int status = 0;

  // clang-format off
  getCommandGate()->runAction(&_doCommand, (void *)16, &status, // NOLINT(readability/casting)
                              provider);
  // clang-format on

  return true;
}

IOReturn AppleIntelWifiAdapterV2::_doCommand(OSObject *target, void *arg0,
                                             void *arg1, void *arg2,
                                             void *arg3) {
  IWL_DEBUG(0, "gatedStart\n");
  AppleIntelWifiAdapterV2 *device =
      reinterpret_cast<AppleIntelWifiAdapterV2 *>(target);
  int *status = reinterpret_cast<int *>(arg1);
  IOService *provider = reinterpret_cast<IOService *>(arg2);

  if (arg0 == (void *)16) {  // NOLINT(readability/casting)
    if (!device->startGated(provider)) {
      OSIncrementAtomic(status);
    }
  } else {
    return kIOReturnError;
  }
  return kIOReturnSuccess;
}

bool AppleIntelWifiAdapterV2::startGated(IOService *provider) {
  int msiIntrIndex = 0;
  for (int index = 0;; index++) {
    int interruptType;
    int ret = provider->getInterruptType(index, &interruptType);
    if (ret != kIOReturnSuccess) break;
    if (interruptType & kIOInterruptTypePCIMessaged) {
      msiIntrIndex = index;
      break;
    }
  }

  IWL_DEBUG(0, "MSI interrupt index: %d\n", msiIntrIndex);

  fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(
      this, (IOInterruptEventAction)&AppleIntelWifiAdapterV2::intrOccured,
      (IOFilterInterruptAction)&AppleIntelWifiAdapterV2::intrFilter, provider,
      msiIntrIndex);
  if (irqLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
    IWL_CRIT(0, "add interrupt event soure fail\n");
    releaseAll();
    return false;
  }

  fInterrupt->enable();

  PMinit();
  provider->joinPMtree(this);
  changePowerStateTo(kOffPowerState);
  registerPowerDriver(this, gPowerStates, kNumPowerStates);

  // setIdleTimerPeriod(iwl_mod_params.d0i3_timeout);

  mediumDict = OSDictionary::withCapacity(MEDIUM_TYPE_INVALID + 1);
  if (!mediumDict) {
    IWL_CRIT(0, "start fail, can not create mediumdict\n");
    releaseAll();
    return false;
  }

  addMediumType(kIOMediumIEEE80211None, 0, MEDIUM_TYPE_NONE);
  addMediumType(kIOMediumIEEE80211Auto, 0, MEDIUM_TYPE_AUTO);
  addMediumType(kIOMediumIEEE80211DS1, 1000000, MEDIUM_TYPE_1MBIT);
  addMediumType(kIOMediumIEEE80211DS2, 2000000, MEDIUM_TYPE_2MBIT);
  addMediumType(kIOMediumIEEE80211DS5, 5500000, MEDIUM_TYPE_5MBIT);
  addMediumType(kIOMediumIEEE80211DS11, 11000000, MEDIUM_TYPE_11MBIT);
  addMediumType(kIOMediumIEEE80211, 54000000, MEDIUM_TYPE_54MBIT, "OFDM54");

  if (!publishMediumDictionary(mediumDict)) {
    IWL_CRIT(0, "start fail, can not publish mediumdict\n");
    releaseAll();
    return false;
  }

  if (!setCurrentMedium(mediumTable[MEDIUM_TYPE_AUTO])) {
    IWL_CRIT(0, "Failed to set current medium!\n");
    releaseAll();
    return false;
  }

  if (!setSelectedMedium(mediumTable[MEDIUM_TYPE_AUTO])) {
    IWL_CRIT(0, "start fail, can not set current medium\n");
    releaseAll();
    return false;
  }

  // for test
  if (!drv->start()) {
    IWL_CRIT(0, "start failed\n");
    releaseAll();
    return false;
  }

  if (!drv->drvStart()) {
    IWL_CRIT(0, "Driver failed to start\n");
    releaseAll();
    return false;
  }

  if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&netif))) {
    IWL_CRIT(0, "start failed, can not attach interface\n");
    releaseAll();
    return false;
  }

  netif->retain();
  netif->registerService();
  registerService();

  drv->trans->m_pDevice->interface = netif;
  return true;
}

bool AppleIntelWifiAdapterV2::intrFilter(OSObject *object,
                                         IOFilterInterruptEventSource *src) {
  AppleIntelWifiAdapterV2 *me =
      reinterpret_cast<AppleIntelWifiAdapterV2 *>(object);

  if (me == 0) return false;

  // kprintf("interrupt filter ran\n");
  me->drv->trans->iwlWrite32(CSR_INT_MASK, 0x00000000);
  return true;
}

void AppleIntelWifiAdapterV2::intrOccured(OSObject *object,
                                          IOInterruptEventSource *sender,
                                          int count) {
  AppleIntelWifiAdapterV2 *o =
      reinterpret_cast<AppleIntelWifiAdapterV2 *>(object);
  if (o == 0) return;

  kprintf("interrupt!!!\n");
  o->drv->irqHandler(0, NULL);
}

bool AppleIntelWifiAdapterV2::configureInterface(
    IONetworkInterface *interface) {
  return super::configureInterface(interface);
}

void AppleIntelWifiAdapterV2::stop(IOService *provider) {
  IWL_DEBUG(0, "Driver Stop()\n");
  drv->m_pDevice->ie_dev->release();
  drv->stopDevice();
  releaseTimeout();
  if (fInterrupt) {
    fInterrupt->disable();
    irqLoop->removeEventSource(fInterrupt);
    fInterrupt->release();
    fInterrupt = NULL;
  }

  if (netif) {
    netif->release();
    detachInterface(netif);
    netif = NULL;
  }
  super::stop(provider);
}

IOReturn AppleIntelWifiAdapterV2::enable(IONetworkInterface *netif) {
  IWL_DEBUG(0, "Driver Enable()\n");
  if (super::enable(netif) != kIOReturnSuccess) {
    IWL_CRIT(0, "super::enable() failed\n");
    return kIOReturnError;
  }

  IOMediumType mediumType = kIOMediumIEEE80211Auto;
  IONetworkMedium *medium =
      IONetworkMedium::getMediumWithType(mediumDict, mediumType);
  setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, medium);
  if (this->drv) {
    if (!this->drv->enableDevice()) {
      IWL_CRIT(0, "Enabling device failed\n");
      return kIOReturnError;
    }

    this->netif->postMessage(1);
    return kIOReturnSuccess;
  } else {
    return kIOReturnError;
  }
}

const OSString *AppleIntelWifiAdapterV2::newModelString() const {
  if (drv)
    return OSString::withCString(
        "Wireless Network Adapter (802.11 a/b/g/n/ac)");

  return OSString::withCString("Wireless Card");
}

const OSString *AppleIntelWifiAdapterV2::newVendorString() const {
  return OSString::withCString("Apple");
}

const OSString *AppleIntelWifiAdapterV2::newRevisionString() const {
  return OSString::withCString("1.0");
}

IOReturn AppleIntelWifiAdapterV2::disable(IONetworkInterface *netif) {
  IWL_DEBUG(0, "Driver Disable()");
  return super::disable(netif);
}

IOReturn AppleIntelWifiAdapterV2::setPromiscuousMode(bool active) {
  return kIOReturnSuccess;
}

IOReturn AppleIntelWifiAdapterV2::setMulticastMode(bool active) {
  return kIOReturnSuccess;
}

IO80211Interface *AppleIntelWifiAdapterV2::getNetworkInterface() {
  return this->netif;
}

IOReturn AppleIntelWifiAdapterV2::getHardwareAddress(IOEthernetAddress *addrP) {
  // Check if address is valid (all zeroes means invalid)
  uint8_t *addr = this->drv->m_pDevice->ie_dev->getMAC();
  if (addr == NULL) return kIOReturnError;

  for (int i = 0; i < ETH_ALEN; i++) {
    if (addr[i]) {
      // Byte is non-zero, this address must be valid and we can return it
      memcpy(&addrP->bytes, addr, ETH_ALEN);

      return kIOReturnSuccess;
    }
  }

  // Address is all zero, return error
  return kIOReturnError;
}

IOReturn AppleIntelWifiAdapterV2::getHardwareAddressForInterface(
    IO80211Interface *netif, IOEthernetAddress *addr) {
  return getHardwareAddress(addr);
}

IOReturn AppleIntelWifiAdapterV2::setMulticastList(IOEthernetAddress *addr,
                                                   UInt32 len) {
  return kIOReturnSuccess;
}

SInt32 AppleIntelWifiAdapterV2::monitorModeSetEnabled(
    IO80211Interface *interface, bool enabled, UInt32 dlt) {
  IWL_INFO(0, "Enable monitor mode\n");
  return kIOReturnSuccess;
}

/*
IOOutputQueue *AppleIntelWifiAdapterV2::createOutputQueue()
{
    if (fOutputQueue == 0) {
        fOutputQueue = IOGatedOutputQueue::withTarget(this, getWorkLoop());
    }
    return fOutputQueue;
}
*/

UInt32 AppleIntelWifiAdapterV2::outputPacket(mbuf_t m, void *param) {
  IWL_INFO(0, "OSX asked us to output a packet\n");

  freePacket(m);
  return 1;
}

IOReturn AppleIntelWifiAdapterV2::getMaxPacketSize(UInt32 *maxSize) const {
  *maxSize = 1500;
  return kIOReturnSuccess;
}

bool AppleIntelWifiAdapterV2::addMediumType(UInt32 type, UInt32 speed,
                                            UInt32 code, char *name) {
  bool ret = false;

  IONetworkMedium *medium = IONetworkMedium::medium(type, speed, 0, code, name);
  if (medium) {
    ret = IONetworkMedium::addMedium(mediumDict, medium);
    if (ret) mediumTable[code] = medium;
    medium->release();
  }
  return ret;
}

int AppleIntelWifiAdapterV2::bpfOutputPacket(OSObject *a2, UInt unk,
                                             mbuf_t packet) {
  IWL_INFO(0, "OSX asked us to output a RAW BPF packet\n");

  freePacket(packet);
  return 1;
}

int AppleIntelWifiAdapterV2::outputRaw80211Packet(IO80211Interface *interface,
                                                  mbuf_t packet) {
  IWL_INFO(0, "OSX asked us to output a raw 80211 packet\n");

  freePacket(packet);
  return 1;
}

int AppleIntelWifiAdapterV2::outputActionFrame(IO80211Interface *interface,
                                               mbuf_t packet) {
  IWL_INFO(0, "OSX asked us to output a raw action frame\n");

  freePacket(packet);
  return 1;
}
