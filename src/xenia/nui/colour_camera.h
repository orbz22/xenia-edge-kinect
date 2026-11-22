/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_COLOUR_CAMERA_H_
#define XENIA_NUI_COLOUR_CAMERA_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace xe {
namespace nui {

// The Kinect colour stream a title asks for: 640x480, two bytes per pixel,
// YUV 4:2:2 (docs/nui.md, "Frame transport").
inline constexpr uint32_t kColourFrameWidth = 640;
inline constexpr uint32_t kColourFrameHeight = 480;
inline constexpr size_t kColourFrameBytes =
    size_t(kColourFrameWidth) * kColourFrameHeight * 2;

// Byte order of one pixel pair of a 4:2:2 frame. The Kinect sensor sends
// U Y0 V Y1 over USB; how the console's camera driver lays the pair out in
// the ring has not been observed, so both orders are offered.
enum class ColourByteOrder {
  kUyvy,
  kYuy2,
};

// Parses the nui_colour_order cvar: "yuy2" (any case) means kYuy2, anything
// else kUyvy.
ColourByteOrder ParseColourByteOrder(const std::string& name);

// Writes one black 4:2:2 frame of `bytes` bytes in the given order.
void FillColourBlack(uint8_t* out, size_t bytes, ColourByteOrder order);

// A host video capture device standing in for the Kinect's colour camera, so
// that a title's photos show the player. Capture runs on a thread of its own,
// and the newest frame is kept scaled to 640x480.
class ColourCamera {
 public:
  // Starts capturing from the first host video capture device whose name
  // contains `device` (ignoring case), or from the first device when `device`
  // is "default". Returns null when the host platform has no capture support.
  // A device that cannot be found or opened is reported in the log, and the
  // camera then simply never has a frame.
  static std::unique_ptr<ColourCamera> Open(const std::string& device);

  virtual ~ColourCamera() = default;

  // Writes the newest frame into `out` as kColourFrameBytes bytes of 640x480
  // YUV 4:2:2 (BT.601, limited range) in `order`, left and right swapped when
  // `mirror` is set. Returns false, leaving `out` alone, before the first frame
  // has arrived.
  virtual bool CopyLatestFrame(uint8_t* out, ColourByteOrder order,
                               bool mirror) = 0;
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_COLOUR_CAMERA_H_
