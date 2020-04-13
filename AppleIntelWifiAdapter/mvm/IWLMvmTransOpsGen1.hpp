//
//  IWLMvmTransOpsGen1.hpp
//  AppleIntelWifiAdapter
//
//  Created by 钟先耀 on 2020/1/13.
//  Copyright © 2020 IntelWifi for MacOS authors. All rights reserved.
//

#ifndef APPLEINTELWIFIADAPTER_MVM_IWLMVMTRANSOPSGEN1_HPP_
#define APPLEINTELWIFIADAPTER_MVM_IWLMVMTRANSOPSGEN1_HPP_

#include "IWLIO.hpp"
#include "IWLTransOps.h"

class IWLMvmTransOpsGen1 : public IWLTransOps {
 public:
  IWLMvmTransOpsGen1() {}
  explicit IWLMvmTransOpsGen1(IWLTransport *trans) : IWLTransOps(trans) {}
  virtual ~IWLMvmTransOpsGen1() {}

  int nicInit() override;

  void fwAlive(UInt32 scd_addr) override;

  int startFW(const struct fw_img *fw, bool run_in_rfkill) override;

  void stopDevice() override;

  void stopDeviceDirectly() override;

  int apmInit() override;

  void apmStop(bool op_mode_leave) override;

  int forcePowerGating() override { return 0; };

 private:
  int txInit();

  int rxInit();

  void setPwr(bool vaux);
};

#endif  // APPLEINTELWIFIADAPTER_MVM_IWLMVMTRANSOPSGEN1_HPP_
