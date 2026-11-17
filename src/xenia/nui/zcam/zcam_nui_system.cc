/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/zcam/zcam_nui_system.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/nui/nui_flags.h"

#include <winsock2.h>
#include <ws2tcpip.h>

DEFINE_int32(nui_zcam_port, 8001,
             "TCP port on localhost where the Z-Cam bridge serves depth "
             "frames for --nui=zcam.",
             "Kinect");

namespace xe {
namespace nui {
namespace zcam {

namespace {

constexpr uint8_t kFrameTypeDepthMm = 3;
constexpr size_t kDepthPayloadBytes =
    size_t(DepthFrame::kWidth) * DepthFrame::kHeight * sizeof(uint16_t);

bool ReadExactly(SOCKET socket, uint8_t* data, size_t size) {
  while (size) {
    const int received =
        recv(socket, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
    if (received <= 0) {
      return false;
    }
    data += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

}  // namespace

bool ZCamNuiSystem::IsAvailable() { return true; }

ZCamNuiSystem::ZCamNuiSystem() = default;

ZCamNuiSystem::~ZCamNuiSystem() = default;

X_STATUS ZCamNuiSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = NuiSystem::Setup(kernel_state);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }
  worker_running_.store(true, std::memory_order_release);
  worker_thread_ =
      threading::Thread::Create({}, [this]() { WorkerThreadMain(); });
  if (!worker_thread_) {
    XELOGE("NUI: failed to start the Z-Cam thread.");
    worker_running_.store(false, std::memory_order_release);
    return X_STATUS_UNSUCCESSFUL;
  }
  worker_thread_->set_name("NUI Z-Cam");
  return X_STATUS_SUCCESS;
}

void ZCamNuiSystem::Shutdown() {
  worker_running_.store(false, std::memory_order_release);
  const uintptr_t socket = socket_.exchange(static_cast<uintptr_t>(~0ull));
  if (socket != static_cast<uintptr_t>(~0ull)) {
    // Unblocks a recv() in the worker.
    closesocket(static_cast<SOCKET>(socket));
  }
  if (worker_thread_) {
    threading::Wait(worker_thread_.get(), false);
    worker_thread_.reset();
  }
  NuiSystem::Shutdown();
}

void ZCamNuiSystem::WorkerThreadMain() {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    XELOGE("NUI: Z-Cam: WSAStartup failed.");
    SetDeviceStatus(DeviceStatus::kError);
    return;
  }

  // The title decides the sensor is there from the device status, so report
  // it ready at once; frames follow when the bridge has any.
  SetDeviceStatus(DeviceStatus::kInitializing);
  SetDeviceStatus(DeviceStatus::kReady);

  bool announced_wait = false;
  uint64_t published = 0;
  std::vector<uint8_t> payload;
  while (worker_running_.load(std::memory_order_acquire)) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(cvars::nui_zcam_port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
      closesocket(socket);
      if (!announced_wait) {
        XELOGI("NUI: Z-Cam: waiting for the bridge on 127.0.0.1:{}",
               cvars::nui_zcam_port);
        announced_wait = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    announced_wait = false;
    XELOGI("NUI: Z-Cam: connected to the bridge");
    socket_.store(static_cast<uintptr_t>(socket));

    while (worker_running_.load(std::memory_order_acquire)) {
      uint8_t header[8];
      if (!ReadExactly(socket, header, sizeof(header))) {
        break;
      }
      uint32_t size;
      std::memcpy(&size, header + 4, sizeof(size));
      if (size > (64u << 20)) {
        XELOGE("NUI: Z-Cam: absurd frame size {}; dropping the connection.",
               size);
        break;
      }
      payload.resize(size);
      if (size && !ReadExactly(socket, payload.data(), size)) {
        break;
      }
      if (header[0] != kFrameTypeDepthMm) {
        continue;
      }
      if (size != kDepthPayloadBytes) {
        XELOGE("NUI: Z-Cam: depth frame of {} bytes, expected {}.", size,
               kDepthPayloadBytes);
        break;
      }
      auto frame = frame_pool_.Acquire();
      // The wire is little endian and so is the host.
      std::memcpy(frame->depth_mm.data(), payload.data(), kDepthPayloadBytes);
      PublishFrame(std::move(frame));
      if ((++published % 300) == 0) {
        XELOGI("NUI: Z-Cam: {} depth frames published", published);
      }
    }

    const uintptr_t current = socket_.exchange(static_cast<uintptr_t>(~0ull));
    if (current != static_cast<uintptr_t>(~0ull)) {
      closesocket(static_cast<SOCKET>(current));
    }
    XELOGI("NUI: Z-Cam: disconnected from the bridge");
  }
  WSACleanup();
}

bool ZCamNuiSystem::GetTiltDegrees(int32_t* out_degrees) {
  if (!out_degrees || !IsDevicePresent()) {
    return false;
  }
  if (GetTiltOverrideDegrees(out_degrees)) {
    return true;
  }
  // No motor: the title is told the angle it last asked for.
  *out_degrees = tilt_degrees_.load(std::memory_order_acquire);
  return true;
}

bool ZCamNuiSystem::SetTiltDegrees(int32_t degrees) {
  if (!IsDevicePresent() || !cvars::nui_allow_tilt) {
    return false;
  }
  tilt_degrees_.store(
      std::clamp(degrees, kTiltMinimumDegrees, kTiltMaximumDegrees),
      std::memory_order_release);
  return true;
}

}  // namespace zcam
}  // namespace nui
}  // namespace xe
