/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_ZCAM_ZCAM_NUI_SYSTEM_H_
#define XENIA_NUI_ZCAM_ZCAM_NUI_SYSTEM_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "xenia/base/threading.h"
#include "xenia/nui/depth_frame_pool.h"
#include "xenia/nui/nui_system.h"

namespace xe {
namespace nui {
namespace zcam {

// A NUI system that publishes the depth frames a Z-Cam bridge serves over a
// local TCP port. Z-Cam turns an ordinary webcam into a depth source; there is
// no tilt motor or accelerometer behind it.
//
// Message from the bridge: u8 type, 3 bytes padding, u32 payload size (little
// endian), then the payload. Type 3 is a 640x480 depth frame of little endian
// 16-bit millimetres, 0 meaning no reading, in the orientation of a real
// sensor (not mirrored). Other types are skipped.
class ZCamNuiSystem : public NuiSystem {
 public:
  ZCamNuiSystem();
  ~ZCamNuiSystem() override;

  // True on Windows, where the TCP client is implemented.
  static bool IsAvailable();

  std::string name() const override { return "Z-Cam bridge"; }

  X_STATUS Setup(kernel::KernelState* kernel_state) override;
  void Shutdown() override;

  bool GetTiltDegrees(int32_t* out_degrees) override;
  bool SetTiltDegrees(int32_t degrees) override;

 private:
  void WorkerThreadMain();

  std::unique_ptr<threading::Thread> worker_thread_;
  std::atomic<bool> worker_running_ = {false};
  // The socket the worker is blocked on, so that Shutdown() can close it.
  std::atomic<uintptr_t> socket_ = {static_cast<uintptr_t>(~0ull)};

  DepthFramePool frame_pool_;
  std::atomic<int32_t> tilt_degrees_ = {0};
};

}  // namespace zcam
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_ZCAM_ZCAM_NUI_SYSTEM_H_
