/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_VMC_VMC_NUI_SYSTEM_H_
#define XENIA_NUI_VMC_VMC_NUI_SYSTEM_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "xenia/base/threading.h"
#include "xenia/nui/depth_frame_pool.h"
#include "xenia/nui/nui_system.h"

namespace xe {
namespace nui {
namespace vmc {

// A NUI system fed by a motion capture application that speaks the VMC
// protocol (OSC over UDP, such as Dollars MoCap). The humanoid skeleton it
// sends is posed with forward kinematics and rendered into a synthetic
// 640x480 depth frame of a body standing in front of a sensor, which the
// title's own tracker then follows. There is no tilt motor behind it.
class VmcNuiSystem : public NuiSystem {
 public:
  VmcNuiSystem();
  ~VmcNuiSystem() override;

  // True on Windows, where the UDP receiver is implemented.
  static bool IsAvailable();

  std::string name() const override { return "VMC motion capture"; }

  X_STATUS Setup(kernel::KernelState* kernel_state) override;
  void Shutdown() override;

  bool GetTiltDegrees(int32_t* out_degrees) override;
  bool SetTiltDegrees(int32_t degrees) override;

 private:
  void WorkerThreadMain();

  std::unique_ptr<threading::Thread> worker_thread_;
  std::atomic<bool> worker_running_ = {false};

  DepthFramePool frame_pool_;
  std::atomic<int32_t> tilt_degrees_ = {0};
};

}  // namespace vmc
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_VMC_VMC_NUI_SYSTEM_H_
