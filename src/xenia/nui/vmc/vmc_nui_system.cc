/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/vmc/vmc_nui_system.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/nui/nui_flags.h"

#include <winsock2.h>
#include <ws2tcpip.h>

DEFINE_int32(nui_vmc_port, 39539,
             "UDP port on localhost where a VMC protocol sender (such as "
             "Dollars MoCap) sends its pose for --nui=vmc.",
             "Kinect");
DEFINE_int32(nui_vmc_distance_mm, 2200,
             "Distance in millimetres from the virtual sensor to the hips of "
             "the rendered body for --nui=vmc.",
             "Kinect");
DEFINE_bool(nui_vmc_mirror, false,
            "Swap left and right in the depth frame rendered for --nui=vmc.",
            "Kinect");
DEFINE_bool(nui_vmc_floor, true,
            "Render a floor plane under the body for --nui=vmc.", "Kinect");

DEFINE_string(nui_vmc_dump, "",
              "File that receives the latest rendered depth frame (640x480 "
              "little endian uint16 millimetres) about once a second, for "
              "--nui=vmc. Empty disables it.",
              "Kinect");

DEFINE_double(nui_vmc_hips_x, 1.0,
              "How much of the sideways movement of the captured hips is "
              "followed for --nui=vmc. 0 pins the body in the middle.",
              "Kinect");
DEFINE_double(nui_vmc_reach, 0.3,
              "Metres a raised hand is pushed toward the sensor, so that it "
              "stands clear of the body in depth for --nui=vmc.",
              "Kinect");
DEFINE_double(nui_vmc_jump_gain, 1.5,
              "How many times the rise of the captured hips above their "
              "resting height is applied to the whole body for --nui=vmc. 0 "
              "disables jumping.",
              "Kinect");
DEFINE_bool(nui_vmc_stand, true,
            "Render the legs standing straight whatever the capture says, so "
            "a player seated at a desk is still a whole body for --nui=vmc.",
            "Kinect");

namespace xe {
namespace nui {
namespace vmc {

namespace {

constexpr int kWidth = DepthFrame::kWidth;
constexpr int kHeight = DepthFrame::kHeight;
// Focal length in pixels of the 57 degree by 43 degree field of view.
constexpr float kFocal = 585.0f;
// Height of the virtual sensor above the floor, in metres.
constexpr float kSensorHeight = 0.95f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 Rotate(Quat q, Vec3 v) {
  const Vec3 u = {q.x, q.y, q.z};
  const Vec3 t = Cross(u, v) * 2.0f;
  return v + t * q.w + Cross(u, t);
}

Quat Multiply(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Normalize(Quat q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1e-6f) {
    return {};
  }
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}

struct Bone {
  Vec3 position;  // Local to the parent, in metres.
  Quat rotation;  // Local to the parent.
};

// Humanoid bones in parent-first order; the parent of the first is none.
enum BoneId {
  kHips,
  kSpine,
  kChest,
  kUpperChest,
  kNeck,
  kHead,
  kLeftShoulder,
  kLeftUpperArm,
  kLeftLowerArm,
  kLeftHand,
  kLeftMiddleProximal,
  kRightShoulder,
  kRightUpperArm,
  kRightLowerArm,
  kRightHand,
  kRightMiddleProximal,
  kLeftUpperLeg,
  kLeftLowerLeg,
  kLeftFoot,
  kLeftToes,
  kRightUpperLeg,
  kRightLowerLeg,
  kRightFoot,
  kRightToes,
  kBoneCount,
};

struct BoneInfo {
  const char* name;
  BoneId parent;
};

constexpr BoneInfo kBones[kBoneCount] = {
    {"Hips", kHips},
    {"Spine", kHips},
    {"Chest", kSpine},
    {"UpperChest", kChest},
    {"Neck", kUpperChest},
    {"Head", kNeck},
    {"LeftShoulder", kUpperChest},
    {"LeftUpperArm", kLeftShoulder},
    {"LeftLowerArm", kLeftUpperArm},
    {"LeftHand", kLeftLowerArm},
    {"LeftMiddleProximal", kLeftHand},
    {"RightShoulder", kUpperChest},
    {"RightUpperArm", kRightShoulder},
    {"RightLowerArm", kRightUpperArm},
    {"RightHand", kRightLowerArm},
    {"RightMiddleProximal", kRightHand},
    {"LeftUpperLeg", kHips},
    {"LeftLowerLeg", kLeftUpperLeg},
    {"LeftFoot", kLeftLowerLeg},
    {"LeftToes", kLeftFoot},
    {"RightUpperLeg", kHips},
    {"RightLowerLeg", kRightUpperLeg},
    {"RightFoot", kRightLowerLeg},
    {"RightToes", kRightFoot},
};

struct Pose {
  std::array<Bone, kBoneCount> bones;
  std::array<bool, kBoneCount> present = {};
};

// OSC reading. Everything is big endian and padded to four bytes.
struct Reader {
  const uint8_t* data;
  size_t size;
  size_t offset = 0;

  bool String(std::string* out) {
    size_t end = offset;
    while (end < size && data[end]) {
      ++end;
    }
    if (end >= size) {
      return false;
    }
    out->assign(reinterpret_cast<const char*>(data + offset), end - offset);
    offset = (end + 4) & ~size_t(3);
    return offset <= size;
  }
  bool Float(float* out) {
    if (offset + 4 > size) {
      return false;
    }
    uint32_t v = (uint32_t(data[offset]) << 24) |
                 (uint32_t(data[offset + 1]) << 16) |
                 (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
    std::memcpy(out, &v, sizeof(v));
    offset += 4;
    return true;
  }
};

void HandleMessage(const uint8_t* data, size_t size, Pose& pose,
                   bool* got_bone) {
  Reader r = {data, size};
  std::string address, types;
  if (!r.String(&address) || !r.String(&types)) {
    return;
  }
  if (address != "/VMC/Ext/Bone/Pos" || types != ",sfffffff") {
    return;
  }
  std::string name;
  float v[7];
  if (!r.String(&name)) {
    return;
  }
  for (float& f : v) {
    if (!r.Float(&f)) {
      return;
    }
  }
  for (int i = 0; i < kBoneCount; ++i) {
    if (name == kBones[i].name) {
      pose.bones[i].position = {v[0], v[1], v[2]};
      pose.bones[i].rotation = Normalize({v[3], v[4], v[5], v[6]});
      pose.present[i] = true;
      *got_bone = true;
      return;
    }
  }
}

void HandlePacket(const uint8_t* data, size_t size, Pose& pose,
                  bool* got_bone) {
  static const char kBundle[] = "#bundle";
  if (size >= 16 && !std::memcmp(data, kBundle, sizeof(kBundle))) {
    size_t offset = 16;
    while (offset + 4 <= size) {
      const uint32_t element = (uint32_t(data[offset]) << 24) |
                               (uint32_t(data[offset + 1]) << 16) |
                               (uint32_t(data[offset + 2]) << 8) |
                               uint32_t(data[offset + 3]);
      offset += 4;
      if (element > size - offset) {
        return;
      }
      HandlePacket(data + offset, element, pose, got_bone);
      offset += element;
    }
    return;
  }
  HandleMessage(data, size, pose, got_bone);
}

// Renders the posed body into a z-buffer in metres along the sensor axis.
struct Renderer {
  std::vector<float> depth = std::vector<float>(kWidth * kHeight, kInfinity);
  float distance;  // Metres from the sensor to the hips.
  float x_offset = 0.0f;  // Sideways shift of the body in the image, metres.
  bool mirror;

  void Clear() { std::fill(depth.begin(), depth.end(), kInfinity); }

  // `p` is a point of the avatar, which faces +Z, in metres.
  Vec3 ToSensor(Vec3 p) const {
    // The avatar faces the sensor, so its +X lies on the image's left.
    const float sx = (mirror ? p.x : -p.x) + x_offset;
    return {sx, p.y - kSensorHeight, distance - p.z};
  }

  void Sphere(Vec3 c, float radius) {
    if (c.z < 0.5f) {
      return;
    }
    const float u = kWidth * 0.5f + kFocal * c.x / c.z;
    const float v = kHeight * 0.5f - kFocal * c.y / c.z;
    const float rp = kFocal * radius / c.z + 1.0f;
    const int x0 = std::max(0, int(std::floor(u - rp)));
    const int x1 = std::min(kWidth - 1, int(std::ceil(u + rp)));
    const int y0 = std::max(0, int(std::floor(v - rp)));
    const int y1 = std::min(kHeight - 1, int(std::ceil(v + rp)));
    const float scale = c.z / kFocal;
    const float r2 = radius * radius;
    for (int y = y0; y <= y1; ++y) {
      const float dy = (v - float(y)) * scale;
      for (int x = x0; x <= x1; ++x) {
        const float dx = (float(x) - u) * scale;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= r2) {
          continue;
        }
        const float z = c.z - std::sqrt(r2 - d2);
        float& slot = depth[y * kWidth + x];
        if (z < slot) {
          slot = z;
        }
      }
    }
  }

  void Capsule(Vec3 a, Vec3 b, float ra, float rb) {
    const Vec3 d = b - a;
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const int steps = std::max(1, int(std::ceil(length / 0.012f)));
    for (int i = 0; i <= steps; ++i) {
      const float t = float(i) / float(steps);
      Sphere(a + d * t, ra + (rb - ra) * t);
    }
  }

  void Floor() {
    for (int y = kHeight / 2 + 1; y < kHeight; ++y) {
      const float z = kSensorHeight * kFocal / float(y - kHeight / 2);
      for (int x = 0; x < kWidth; ++x) {
        float& slot = depth[y * kWidth + x];
        if (z < slot) {
          slot = z;
        }
      }
    }
  }
};

void Render(const Pose& pose, Renderer& renderer, bool stand, float lift) {
  Vec3 position[kBoneCount];
  Quat rotation[kBoneCount];
  bool valid[kBoneCount];
  for (int i = 0; i < kBoneCount; ++i) {
    valid[i] = pose.present[i];
    if (i == kHips) {
      // The capture's depth drift would move the player toward and away from
      // the sensor, so only the sideways step and the height are kept.
      position[i] = {std::clamp(pose.bones[i].position.x *
                                    float(cvars::nui_vmc_hips_x),
                                -0.8f, 0.8f),
                     pose.bones[i].position.y, 0};
      if (stand && pose.present[kLeftUpperLeg] && pose.present[kLeftLowerLeg] &&
          pose.present[kLeftFoot]) {
        // Standing straight, the feet rest on the floor whatever height the
        // capture reports for the hips.
        const Vec3 thigh = pose.bones[kLeftLowerLeg].position;
        const Vec3 shin = pose.bones[kLeftFoot].position;
        position[i].y = std::sqrt(thigh.x * thigh.x + thigh.y * thigh.y +
                                  thigh.z * thigh.z) +
                        std::sqrt(shin.x * shin.x + shin.y * shin.y +
                                  shin.z * shin.z) -
                        pose.bones[kLeftUpperLeg].position.y + 0.09f;
      }
      rotation[i] = pose.bones[i].rotation;
      continue;
    }
    const int parent = kBones[i].parent;
    valid[i] = valid[i] && valid[parent];
    if (!valid[i]) {
      continue;
    }
    position[i] =
        position[parent] + Rotate(rotation[parent], pose.bones[i].position);
    rotation[i] = Multiply(rotation[parent], pose.bones[i].rotation);
  }
  // Without the hips there is nothing to place.
  if (!valid[kHips]) {
    return;
  }
  if (stand) {
    // Hang straight legs below the hip joints, with the bone lengths of the
    // capture and the feet flat.
    const BoneId legs[2][4] = {
        {kLeftUpperLeg, kLeftLowerLeg, kLeftFoot, kLeftToes},
        {kRightUpperLeg, kRightLowerLeg, kRightFoot, kRightToes}};
    for (const auto& leg : legs) {
      if (!valid[leg[0]] || !valid[leg[1]] || !valid[leg[2]]) {
        continue;
      }
      const Vec3 thigh = pose.bones[leg[1]].position;
      const Vec3 shin = pose.bones[leg[2]].position;
      position[leg[1]] =
          position[leg[0]] -
          Vec3{0, std::sqrt(thigh.x * thigh.x + thigh.y * thigh.y +
                            thigh.z * thigh.z),
               0};
      position[leg[2]] =
          position[leg[1]] -
          Vec3{0, std::sqrt(shin.x * shin.x + shin.y * shin.y + shin.z * shin.z),
               0};
      if (valid[leg[3]]) {
        position[leg[3]] = position[leg[2]] + pose.bones[leg[3]].position;
      }
    }
  }
  for (int i = 0; i < kBoneCount; ++i) {
    position[i].y += lift;
  }
  // Raised hands come toward the sensor so they are separate from the torso.
  const float reach = float(cvars::nui_vmc_reach);
  if (reach > 0.0f) {
    const BoneId arms[2][4] = {
        {kLeftLowerArm, kLeftHand, kLeftMiddleProximal, kLeftUpperArm},
        {kRightLowerArm, kRightHand, kRightMiddleProximal, kRightUpperArm}};
    for (const auto& arm : arms) {
      if (!valid[arm[1]]) {
        continue;
      }
      const float raise = std::clamp(
          (position[arm[1]].y - (position[kHips].y + 0.15f)) / 0.5f, 0.0f,
          1.0f);
      position[arm[0]].z += reach * raise * 0.6f;
      position[arm[1]].z += reach * raise;
      position[arm[2]].z += reach * raise;
    }
  }
  // Keep the body's own height; only the sideways and depth offset move it.
  Vec3 world[kBoneCount];
  for (int i = 0; i < kBoneCount; ++i) {
    world[i] = renderer.ToSensor(position[i]);
  }
  auto capsule = [&](BoneId a, BoneId b, float ra, float rb) {
    if (valid[a] && valid[b]) {
      renderer.Capsule(world[a], world[b], ra, rb);
    }
  };
  // Torso and pelvis.
  capsule(kHips, kSpine, 0.12f, 0.13f);
  capsule(kSpine, kChest, 0.13f, 0.14f);
  capsule(kChest, kUpperChest, 0.14f, 0.14f);
  capsule(kUpperChest, kNeck, 0.14f, 0.05f);
  capsule(kLeftUpperLeg, kRightUpperLeg, 0.09f, 0.09f);
  capsule(kLeftUpperArm, kRightUpperArm, 0.07f, 0.07f);
  // Head: a sphere above the head bone, along the head's up axis.
  if (valid[kHead]) {
    const Vec3 up = Rotate(rotation[kHead], {0, 0.09f, 0});
    renderer.Sphere(renderer.ToSensor(position[kHead] + up), 0.105f);
    capsule(kNeck, kHead, 0.05f, 0.05f);
  }
  // Arms.
  capsule(kLeftShoulder, kLeftUpperArm, 0.05f, 0.05f);
  capsule(kLeftUpperArm, kLeftLowerArm, 0.05f, 0.04f);
  capsule(kLeftLowerArm, kLeftHand, 0.04f, 0.035f);
  capsule(kLeftHand, kLeftMiddleProximal, 0.04f, 0.03f);
  capsule(kRightShoulder, kRightUpperArm, 0.05f, 0.05f);
  capsule(kRightUpperArm, kRightLowerArm, 0.05f, 0.04f);
  capsule(kRightLowerArm, kRightHand, 0.04f, 0.035f);
  capsule(kRightHand, kRightMiddleProximal, 0.04f, 0.03f);
  // Legs.
  capsule(kLeftUpperLeg, kLeftLowerLeg, 0.08f, 0.055f);
  capsule(kLeftLowerLeg, kLeftFoot, 0.055f, 0.045f);
  capsule(kLeftFoot, kLeftToes, 0.045f, 0.035f);
  capsule(kRightUpperLeg, kRightLowerLeg, 0.08f, 0.055f);
  capsule(kRightLowerLeg, kRightFoot, 0.055f, 0.045f);
  capsule(kRightFoot, kRightToes, 0.045f, 0.035f);
}

}  // namespace

bool VmcNuiSystem::IsAvailable() { return true; }

VmcNuiSystem::VmcNuiSystem() = default;

VmcNuiSystem::~VmcNuiSystem() = default;

X_STATUS VmcNuiSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = NuiSystem::Setup(kernel_state);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }
  worker_running_.store(true, std::memory_order_release);
  worker_thread_ =
      threading::Thread::Create({}, [this]() { WorkerThreadMain(); });
  if (!worker_thread_) {
    XELOGE("NUI: failed to start the VMC thread.");
    worker_running_.store(false, std::memory_order_release);
    return X_STATUS_UNSUCCESSFUL;
  }
  worker_thread_->set_name("NUI VMC");
  return X_STATUS_SUCCESS;
}

void VmcNuiSystem::Shutdown() {
  worker_running_.store(false, std::memory_order_release);
  if (worker_thread_) {
    threading::Wait(worker_thread_.get(), false);
    worker_thread_.reset();
  }
  NuiSystem::Shutdown();
}

namespace {

// One captured person: the sender's latest pose and the state that turns the
// captured hips into a jump.
struct Player {
  SOCKET socket = INVALID_SOCKET;
  Pose pose;
  std::chrono::steady_clock::time_point last_bone;
  float rest_hips_y = 0.9f;
  bool have_rest = false;
  float hips_min = 1e9f, hips_max = -1e9f;
};

}  // namespace

void VmcNuiSystem::WorkerThreadMain() {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    XELOGE("NUI: VMC: WSAStartup failed.");
    SetDeviceStatus(DeviceStatus::kError);
    return;
  }
  // The second player sends to the port after the first.
  constexpr int kPlayerCount = 2;
  Player players[kPlayerCount];
  for (int i = 0; i < kPlayerCount; ++i) {
    const int port = cvars::nui_vmc_port + i;
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (socket == INVALID_SOCKET ||
        bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
            0) {
      XELOGE("NUI: VMC: cannot listen on UDP 127.0.0.1:{}.", port);
      if (socket != INVALID_SOCKET) {
        closesocket(socket);
      }
      continue;
    }
    const DWORD timeout_ms = 2;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    players[i].socket = socket;
    XELOGI("NUI: VMC: player {} listens on UDP 127.0.0.1:{}", i + 1, port);
  }
  if (players[0].socket == INVALID_SOCKET) {
    SetDeviceStatus(DeviceStatus::kError);
    for (Player& player : players) {
      if (player.socket != INVALID_SOCKET) {
        closesocket(player.socket);
      }
    }
    WSACleanup();
    return;
  }

  // The title decides the sensor is there from the device status.
  SetDeviceStatus(DeviceStatus::kInitializing);
  SetDeviceStatus(DeviceStatus::kReady);

  using Clock = std::chrono::steady_clock;
  const auto frame_period = std::chrono::milliseconds(33);
  auto next_frame = Clock::now();
  uint64_t published = 0;
  Clock::duration render_time = {};
  Renderer renderer;
  std::vector<uint8_t> packet(65536);
  while (worker_running_.load(std::memory_order_acquire)) {
    for (int i = 0; i < kPlayerCount; ++i) {
      Player& player = players[i];
      if (player.socket == INVALID_SOCKET) {
        continue;
      }
      const int received =
          recv(player.socket, reinterpret_cast<char*>(packet.data()),
               static_cast<int>(packet.size()), 0);
      if (received > 0) {
        bool got_bone = false;
        HandlePacket(packet.data(), size_t(received), player.pose, &got_bone);
        if (got_bone) {
          if (player.last_bone == Clock::time_point()) {
            XELOGI("NUI: VMC: receiving the pose of player {}", i + 1);
          }
          player.last_bone = Clock::now();
        }
      }
    }
    const auto now = Clock::now();
    if (now < next_frame) {
      continue;
    }
    next_frame = now + frame_period;
    auto frame = frame_pool_.Acquire();
    bool fresh[kPlayerCount];
    int fresh_count = 0;
    for (int i = 0; i < kPlayerCount; ++i) {
      fresh[i] = players[i].last_bone != Clock::time_point() &&
                 now - players[i].last_bone < std::chrono::milliseconds(500);
      fresh_count += fresh[i] ? 1 : 0;
    }
    renderer.distance = float(cvars::nui_vmc_distance_mm) / 1000.0f;
    renderer.mirror = cvars::nui_vmc_mirror;
    if (fresh_count) {
      const auto render_start = Clock::now();
      renderer.Clear();
      for (int i = 0; i < kPlayerCount; ++i) {
        if (!fresh[i]) {
          continue;
        }
        Player& player = players[i];
        // A jump shows as the captured hips rising above their resting
        // height, which is tracked as a slowly adapting average.
        float lift = 0.0f;
        if (player.pose.present[kHips]) {
          const float hips_y = player.pose.bones[kHips].position.y;
          if (!player.have_rest) {
            player.rest_hips_y = hips_y;
            player.have_rest = true;
          }
          player.rest_hips_y +=
              (hips_y - player.rest_hips_y) *
              (hips_y < player.rest_hips_y ? 0.05f : 0.004f);
          lift = std::clamp(
              (hips_y - player.rest_hips_y) * float(cvars::nui_vmc_jump_gain),
              0.0f, 0.8f);
          player.hips_min = std::min(player.hips_min, hips_y);
          player.hips_max = std::max(player.hips_max, hips_y);
        }
        // Two players stand side by side, one alone stands in the middle.
        renderer.x_offset = fresh_count == 1 ? 0.0f : (i == 0 ? -0.5f : 0.5f);
        Render(player.pose, renderer, cvars::nui_vmc_stand, lift);
      }
      if (cvars::nui_vmc_floor) {
        renderer.Floor();
      }
      render_time += Clock::now() - render_start;
      for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i) {
        const float z = renderer.depth[i];
        // The tracker reads 0.8 m to 4 m; beyond that is no reading.
        frame->depth_mm[i] =
            z > 4.0f ? 0 : uint16_t(std::max(z * 1000.0f, 1.0f));
      }
    } else {
      std::fill(frame->depth_mm.begin(), frame->depth_mm.end(), uint16_t(0));
    }
    if (!cvars::nui_vmc_dump.empty() && (published % 30) == 0) {
      if (FILE* dump = std::fopen(cvars::nui_vmc_dump.c_str(), "wb")) {
        std::fwrite(frame->depth_mm.data(), sizeof(uint16_t),
                    size_t(kWidth) * kHeight, dump);
        std::fclose(dump);
      }
    }
    PublishFrame(std::move(frame));
    if ((++published % 300) == 0) {
      XELOGI(
          "NUI: VMC: {} depth frames published, render {:.1f} ms, players "
          "{}, captured hips y {:.3f}..{:.3f} rest {:.3f}",
          published,
          std::chrono::duration<double, std::milli>(render_time).count() / 300,
          fresh_count, players[0].hips_min, players[0].hips_max,
          players[0].rest_hips_y);
      render_time = {};
      for (Player& player : players) {
        player.hips_min = 1e9f;
        player.hips_max = -1e9f;
      }
    }
  }
  for (Player& player : players) {
    if (player.socket != INVALID_SOCKET) {
      closesocket(player.socket);
    }
  }
  WSACleanup();
}

bool VmcNuiSystem::GetTiltDegrees(int32_t* out_degrees) {
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

bool VmcNuiSystem::SetTiltDegrees(int32_t degrees) {
  if (!IsDevicePresent() || !cvars::nui_allow_tilt) {
    return false;
  }
  tilt_degrees_.store(
      std::clamp(degrees, kTiltMinimumDegrees, kTiltMaximumDegrees),
      std::memory_order_release);
  return true;
}

}  // namespace vmc
}  // namespace nui
}  // namespace xe
