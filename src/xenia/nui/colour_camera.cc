/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/colour_camera.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace nui {

namespace {

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

// BT.601 limited range, in 8.8 fixed point.
inline uint8_t RgbToY(int r, int g, int b) {
  return uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}
inline uint8_t RgbToU(int r, int g, int b) {
  return uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}
inline uint8_t RgbToV(int r, int g, int b) {
  return uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

// Converts a 640x480 frame of 0x00RRGGBB pixels into 4:2:2.
void ConvertRgbToColourFrame(const uint32_t* rgb, uint8_t* out,
                             ColourByteOrder order, bool mirror) {
  for (uint32_t y = 0; y < kColourFrameHeight; ++y) {
    const uint32_t* row = rgb + size_t(y) * kColourFrameWidth;
    uint8_t* dest = out + size_t(y) * kColourFrameWidth * 2;
    for (uint32_t x = 0; x < kColourFrameWidth; x += 2) {
      const uint32_t p0 = mirror ? row[kColourFrameWidth - 1 - x] : row[x];
      const uint32_t p1 = mirror ? row[kColourFrameWidth - 2 - x] : row[x + 1];
      const int r0 = (p0 >> 16) & 0xFF, g0 = (p0 >> 8) & 0xFF, b0 = p0 & 0xFF;
      const int r1 = (p1 >> 16) & 0xFF, g1 = (p1 >> 8) & 0xFF, b1 = p1 & 0xFF;
      const int r = (r0 + r1) >> 1, g = (g0 + g1) >> 1, b = (b0 + b1) >> 1;
      const uint8_t y0 = RgbToY(r0, g0, b0);
      const uint8_t y1 = RgbToY(r1, g1, b1);
      const uint8_t u = RgbToU(r, g, b);
      const uint8_t v = RgbToV(r, g, b);
      if (order == ColourByteOrder::kUyvy) {
        dest[0] = u;
        dest[1] = y0;
        dest[2] = v;
        dest[3] = y1;
      } else {
        dest[0] = y0;
        dest[1] = u;
        dest[2] = y1;
        dest[3] = v;
      }
      dest += 4;
    }
  }
}

#if XE_PLATFORM_WIN32

using Microsoft::WRL::ComPtr;

// MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE from the Windows 11 SDK, which
// only declares it for newer NTDDI_VERSION targets: set to 1 on a capture
// device's activation object, it opens the camera as a shared reader of the
// stream another program already runs, in that program's format.
constexpr GUID kFrameServerShareMode = {
    0x44d1a9bc, 0x2999, 0x4238, {0xae, 0x43, 0x07, 0x30, 0xce, 0xb2, 0xab, 0x1b}};

// Captures through a Media Foundation source reader, which decodes whatever
// the device delivers (MJPEG included) into RGB32.
class MfColourCamera final : public ColourCamera {
 public:
  explicit MfColourCamera(std::string device) : device_(std::move(device)) {
    rgb_.resize(size_t(kColourFrameWidth) * kColourFrameHeight);
    thread_ = std::thread([this] { Run(); });
  }

  ~MfColourCamera() override {
    {
      std::lock_guard<std::mutex> lock(source_mutex_);
      stopping_ = true;
      // A synchronous ReadSample only comes back with a frame or an error,
      // and shutting the source down is what turns it into an error.
      if (source_) {
        source_->Shutdown();
      }
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool CopyLatestFrame(uint8_t* out, ColourByteOrder order,
                       bool mirror) override {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!have_frame_) {
      return false;
    }
    ConvertRgbToColourFrame(rgb_.data(), out, order, mirror);
    return true;
  }

 private:
  void Run() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      Capture();
      MFShutdown();
    } else {
      XELOGE("NUI: colour camera: Media Foundation could not be started");
    }
    if (SUCCEEDED(com_result)) {
      CoUninitialize();
    }
  }

  bool ReadOutputType(IMFSourceReader* reader) {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(
            DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)) ||
        FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width_,
                                  &height_)) ||
        !width_ || !height_) {
      return false;
    }
    UINT32 stride = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
      stride_ = int32_t(stride);
    } else {
      stride_ = int32_t(width_ * 4);
    }
    // Take the centre of the picture at the 4:3 aspect of the Kinect's colour
    // camera, so that a widescreen webcam is cropped rather than squashed.
    crop_x_ = crop_y_ = 0;
    crop_width_ = width_;
    crop_height_ = height_;
    if (uint64_t(width_) * 3 > uint64_t(height_) * 4) {
      crop_width_ = height_ * 4 / 3;
      crop_x_ = (width_ - crop_width_) / 2;
    } else if (uint64_t(width_) * 3 < uint64_t(height_) * 4) {
      crop_height_ = width_ * 3 / 4;
      crop_y_ = (height_ - crop_height_) / 2;
    }
    return true;
  }

  // Finds the device the nui_colour_camera cvar names.
  ComPtr<IMFActivate> FindDevice(std::string* out_name) {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1)) ||
        FAILED(attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
      return nullptr;
    }
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count))) {
      XELOGE("NUI: colour camera: the video capture devices cannot be listed");
      return nullptr;
    }
    const std::string wanted = ToLower(device_);
    const bool take_first = wanted == "default";
    ComPtr<IMFActivate> found;
    std::string names;
    for (UINT32 i = 0; i < count; ++i) {
      std::string name;
      WCHAR* friendly_name = nullptr;
      UINT32 length = 0;
      if (SUCCEEDED(devices[i]->GetAllocatedString(
              MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly_name,
              &length))) {
        name = xe::to_utf8(std::u16string_view(
            reinterpret_cast<const char16_t*>(friendly_name), length));
        CoTaskMemFree(friendly_name);
      }
      names += names.empty() ? "" : ", ";
      names += "\"" + name + "\"";
      if (!found &&
          (take_first || ToLower(name).find(wanted) != std::string::npos)) {
        found = devices[i];
        *out_name = name;
      }
      devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (!found) {
      XELOGW("NUI: colour camera: no video capture device matches \"{}\"; "
             "found {}",
             device_, names.empty() ? std::string("none") : names);
    }
    return found;
  }

  // Activates the device and makes a source reader that delivers RGB32 from
  // the device's native format `native_index`, or from the format the device
  // already has for -1. Returns null on failure.
  ComPtr<IMFSourceReader> OpenReader(IMFActivate* device, int native_index) {
    ComPtr<IMFMediaSource> source;
    if (FAILED(device->ActivateObject(IID_PPV_ARGS(&source)))) {
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(source_mutex_);
      if (stopping_) {
        source->Shutdown();
        return nullptr;
      }
      source_ = source;
    }
    ComPtr<IMFAttributes> reader_attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> output_type;
    bool ready =
        SUCCEEDED(MFCreateAttributes(&reader_attributes, 1)) &&
        SUCCEEDED(reader_attributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
        SUCCEEDED(MFCreateSourceReaderFromMediaSource(
            source.Get(), reader_attributes.Get(), &reader));
    if (ready) {
      reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
      reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                 TRUE);
      if (native_index >= 0) {
        ComPtr<IMFMediaType> native_type;
        ready = SUCCEEDED(reader->GetNativeMediaType(
                    DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                    DWORD(native_index), &native_type)) &&
                SUCCEEDED(reader->SetCurrentMediaType(
                    DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                    native_type.Get()));
      }
    }
    ready = ready && SUCCEEDED(MFCreateMediaType(&output_type)) &&
            SUCCEEDED(
                output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
            SUCCEEDED(
                output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
            SUCCEEDED(reader->SetCurrentMediaType(
                DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                output_type.Get())) &&
            ReadOutputType(reader.Get());
    if (!ready) {
      CloseReader(device, reader);
      return nullptr;
    }
    return reader;
  }

  void CloseReader(IMFActivate* device, ComPtr<IMFSourceReader>& reader) {
    reader.Reset();
    {
      std::lock_guard<std::mutex> lock(source_mutex_);
      if (source_) {
        source_->Shutdown();
        source_.Reset();
      }
    }
    device->ShutdownObject();
  }

  // The native formats worth trying, one per subtype and frame size, as
  // indices for OpenReader.
  static std::vector<int> ListNativeTypes(IMFSourceReader* reader) {
    std::vector<int> indices;
    std::vector<std::pair<GUID, uint64_t>> seen;
    for (DWORD i = 0; indices.size() < 32; ++i) {
      ComPtr<IMFMediaType> type;
      if (FAILED(reader->GetNativeMediaType(
              DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type))) {
        break;
      }
      GUID subtype = {};
      UINT32 width = 0, height = 0;
      if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
          FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width,
                                    &height))) {
        continue;
      }
      const auto key = std::make_pair(subtype, (uint64_t(width) << 32) | height);
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
        continue;
      }
      seen.push_back(key);
      indices.push_back(int(i));
    }
    return indices;
  }

  // Reads frames until the first one arrives. A camera that Windows shares
  // between programs only serves the format the first of them chose; any
  // other format fails here with MF_E_HW_MFT_FAILED_START_STREAMING.
  HRESULT ReadFirstFrame(IMFSourceReader* reader, uint32_t* scaled) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      DWORD stream_index = 0, flags = 0;
      LONGLONG timestamp = 0;
      ComPtr<IMFSample> sample;
      const HRESULT result = reader->ReadSample(
          DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream_index, &flags,
          &timestamp, &sample);
      if (FAILED(result)) {
        return result;
      }
      if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
        return E_FAIL;
      }
      if (sample && ScaleSample(sample.Get(), scaled)) {
        return S_OK;
      }
    }
    return E_FAIL;
  }

  void PublishFrame(std::vector<uint32_t>& scaled) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    rgb_.swap(scaled);
    have_frame_ = true;
  }

  void Capture() {
    std::string name;
    ComPtr<IMFActivate> device = FindDevice(&name);
    if (!device) {
      return;
    }
    std::vector<uint32_t> scaled(size_t(kColourFrameWidth) *
                                 kColourFrameHeight);

    // First as a shared reader of whatever another program already streams,
    // which leaves that program's format alone; then on our own, in the
    // format the device already has and then in every native format, until
    // one delivers.
    device->SetUINT32(kFrameServerShareMode, 1);
    ComPtr<IMFSourceReader> reader = OpenReader(device.Get(), -1);
    HRESULT result = reader ? ReadFirstFrame(reader.Get(), scaled.data())
                            : HRESULT(E_FAIL);
    bool shared = SUCCEEDED(result);
    std::vector<int> candidates;
    if (!shared) {
      CloseReader(device.Get(), reader);
      device->DeleteItem(kFrameServerShareMode);
      reader = OpenReader(device.Get(), -1);
      if (reader) {
        candidates = ListNativeTypes(reader.Get());
      }
      result = reader ? ReadFirstFrame(reader.Get(), scaled.data())
                      : HRESULT(E_FAIL);
    }
    for (size_t i = 0; FAILED(result) && i < candidates.size(); ++i) {
      CloseReader(device.Get(), reader);
      {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (stopping_) {
          return;
        }
      }
      reader = OpenReader(device.Get(), candidates[i]);
      result = reader ? ReadFirstFrame(reader.Get(), scaled.data())
                      : HRESULT(E_FAIL);
    }
    if (FAILED(result)) {
      std::lock_guard<std::mutex> lock(source_mutex_);
      if (!stopping_) {
        XELOGE("NUI: colour camera: \"{}\" delivers no frames in any of its "
               "formats ({:08X}); another program may hold it exclusively",
               name, uint32_t(result));
      }
    } else {
      XELOGI("NUI: colour camera: \"{}\" delivers {}x{}{}, {}x{} of it "
             "scaled to {}x{}",
             name, width_, height_,
             shared ? " shared with another program" : "", crop_width_,
             crop_height_, kColourFrameWidth, kColourFrameHeight);
      PublishFrame(scaled);
      scaled.resize(size_t(kColourFrameWidth) * kColourFrameHeight);
    }

    while (SUCCEEDED(result)) {
      DWORD stream_index = 0, flags = 0;
      LONGLONG timestamp = 0;
      ComPtr<IMFSample> sample;
      const HRESULT read_result = reader->ReadSample(
          DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream_index, &flags,
          &timestamp, &sample);
      if (FAILED(read_result) ||
          (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (!stopping_) {
          XELOGW("NUI: colour camera: capture stopped ({:08X}, flags {:X})",
                 uint32_t(read_result), uint32_t(flags));
        }
        break;
      }
      if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) &&
          !ReadOutputType(reader.Get())) {
        break;
      }
      if (sample && ScaleSample(sample.Get(), scaled.data())) {
        PublishFrame(scaled);
      }
    }
    CloseReader(device.Get(), reader);
  }

  // Nearest-neighbour scales one RGB32 sample into a 640x480 frame.
  bool ScaleSample(IMFSample* sample, uint32_t* out) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
      return false;
    }
    ComPtr<IMF2DBuffer> buffer_2d;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    BYTE* data = nullptr;
    bool locked_2d = false;
    if (SUCCEEDED(buffer.As(&buffer_2d)) &&
        SUCCEEDED(buffer_2d->Lock2D(&scan0, &pitch))) {
      locked_2d = true;
    } else {
      DWORD length = 0;
      if (FAILED(buffer->Lock(&data, nullptr, &length))) {
        return false;
      }
      if (length < size_t(std::abs(stride_)) * height_) {
        buffer->Unlock();
        return false;
      }
      pitch = stride_;
      scan0 = stride_ < 0 ? data + size_t(-stride_) * (height_ - 1) : data;
    }
    for (uint32_t y = 0; y < kColourFrameHeight; ++y) {
      const uint32_t source_y = crop_y_ + y * crop_height_ / kColourFrameHeight;
      const auto* row =
          reinterpret_cast<const uint32_t*>(scan0 + ptrdiff_t(pitch) * source_y);
      uint32_t* dest = out + size_t(y) * kColourFrameWidth;
      for (uint32_t x = 0; x < kColourFrameWidth; ++x) {
        dest[x] =
            row[crop_x_ + x * crop_width_ / kColourFrameWidth] & 0x00FFFFFF;
      }
    }
    if (locked_2d) {
      buffer_2d->Unlock2D();
    } else {
      buffer->Unlock();
    }
    return true;
  }

  const std::string device_;
  std::thread thread_;

  std::mutex source_mutex_;
  ComPtr<IMFMediaSource> source_;
  bool stopping_ = false;

  // Owned by the capture thread.
  UINT32 width_ = 0;
  UINT32 height_ = 0;
  int32_t stride_ = 0;
  // The 4:3 part of the picture that is scaled into the frame.
  UINT32 crop_x_ = 0;
  UINT32 crop_y_ = 0;
  UINT32 crop_width_ = 0;
  UINT32 crop_height_ = 0;

  std::mutex frame_mutex_;
  std::vector<uint32_t> rgb_;
  bool have_frame_ = false;
};

#endif  // XE_PLATFORM_WIN32

}  // namespace

ColourByteOrder ParseColourByteOrder(const std::string& name) {
  return ToLower(name) == "yuy2" ? ColourByteOrder::kYuy2
                                 : ColourByteOrder::kUyvy;
}

void FillColourBlack(uint8_t* out, size_t bytes, ColourByteOrder order) {
  // Black is a luma of 16 with both chroma channels at 128.
  const uint8_t even = order == ColourByteOrder::kUyvy ? 0x80 : 0x10;
  const uint8_t odd = order == ColourByteOrder::kUyvy ? 0x10 : 0x80;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    out[i] = even;
    out[i + 1] = odd;
  }
}

std::unique_ptr<ColourCamera> ColourCamera::Open(const std::string& device) {
#if XE_PLATFORM_WIN32
  return std::make_unique<MfColourCamera>(device);
#else
  XELOGW("NUI: colour camera: host video capture is only supported on "
         "Windows");
  return nullptr;
#endif  // XE_PLATFORM_WIN32
}

}  // namespace nui
}  // namespace xe
