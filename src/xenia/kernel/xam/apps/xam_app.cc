/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xam_app.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/nui_messages.h"
#include "xenia/kernel/xam/nui_state.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xenumerator.h"

DECLARE_string(media_type);

/* Notes:
   - Messages ids that start with 0x00021xxx are UI calls
   - Messages ids that start with 0x00023xxx are used for the user profile
   - Messages ids that start with 0x0002Bxxx are used by the Kinect device
   usually for camera related functions
   - Messages ids that start with 0x0002Cxxx are used by the XamNuiIdentity
   functions
*/

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XamApp::XamApp(KernelState* kernel_state) : App(kernel_state, 0xFE) {}

X_HRESULT XamApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // Everything the Kinect sends goes to the NUI state, which knows about the
  // host sensor and logs whatever it does not understand yet. The identity
  // subsystem also starts I/O requests outside those two ranges, which reach
  // here through XMsgStartIORequest.
  if (IsNuiXamMessage(message)) {
    return kernel_state_->xam_state()->nui_state()->DispatchNuiMessage(
        message, buffer_ptr, buffer_length);
  }

  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0002000E: {
      X_ENUMERATE_PARAM* data_ptr =
          reinterpret_cast<X_ENUMERATE_PARAM*>(buffer);

      XELOGD(
          "XEnumerateCrossTitle({:04X}, {:04X}, {:04X}, {:04X}, {}, {}, "
          "{:04X})",
          data_ptr->user_index.get(), data_ptr->flags.get(),
          data_ptr->private_enum_structure_ptr.get(),
          data_ptr->buffer_ptr.get(), data_ptr->buffer_size.get(),
          data_ptr->items_requested.get(), data_ptr->items_returned_ptr.get());

      if (!data_ptr->buffer_ptr || !data_ptr->private_enum_structure_ptr) {
        return X_E_INVALIDARG;
      }

      auto enum_struct =
          memory_->TranslateVirtual<X_KENUMERATOR_CONTENT_AGGREGATE*>(
              data_ptr->private_enum_structure_ptr);

      auto e = kernel_state_->object_table()->LookupObject<XEnumerator>(
          enum_struct->handle);

      if (!e) {
        return X_E_INVALIDARG;
      }

      assert_true(enum_struct->magic == kXObjSignature);

      // The enumerator writes the items as XCONTENT_DATA_INTERNAL.
      uint8_t* content_data_ptr =
          memory_->TranslateVirtual<uint8_t*>(data_ptr->buffer_ptr);

      assert_true(data_ptr->buffer_size == sizeof(XCONTENT_DATA_INTERNAL));

      std::memset(content_data_ptr, 0, data_ptr->buffer_size);

      uint32_t item_count = 0;
      X_RESULT result = e->WriteItems(content_data_ptr, data_ptr->buffer_size,
                                      &item_count, data_ptr->flags);

      result = X_HRESULT_FROM_WIN32(result);

      xe::be<uint32_t>* items_returned_ptr =
          memory_->TranslateVirtual<xe::be<uint32_t>*>(
              data_ptr->items_returned_ptr);

      *items_returned_ptr = item_count;

      return result;
    }
    case 0x00020021: {
      struct XContentQueryVolumeDeviceType {
        char root_name[64];
        xe::be<uint32_t> is_title_process;
        xe::be<DeviceType> device_type_ptr;
        xe::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XContentQueryVolumeDeviceType*>(buffer);
      assert_true(buffer_length == sizeof(XContentQueryVolumeDeviceType));

      std::string target;
      if (!kernel_state_->file_system()->FindSymbolicLink(
              std::string(data->root_name) + ':', target)) {
        return X_E_INVALIDARG;
      }

      // The answer is where the volume lives, not the deployment type. A disc
      // rip installed to the HDD runs as a disc title from an HDD volume.
      // media_type picks the drive the launch volume reports.
      const bool on_package =
          xe::utf8::starts_with_case(target, "\\Device\\Package_");
      bool on_hdd = xe::utf8::starts_with_case(target, "\\Device\\Content\\");
      if (on_package ||
          xe::utf8::starts_with_case(target, "\\Device\\Cdrom0")) {
        on_hdd = cvars::media_type == "hdd" ||
                 (cvars::media_type != "odd" && on_package);
      }

      // A disc answers like any other root.
      if (!on_hdd) {
        return X_E_INVALIDARG;
      }

      xe::be<DeviceType>* device_type_ptr =
          memory_->TranslateVirtual<xe::be<DeviceType>*>(
              static_cast<uint32_t>(data->device_type_ptr.get()));
      *device_type_ptr = DeviceType::HDD;

      XELOGD("XContentQueryVolumeDeviceType('{}', {:08X}, {:08X}, {:08X})",
             data->root_name,
             static_cast<uint32_t>(data->is_title_process.get()),
             static_cast<uint32_t>(data->device_type_ptr.get()),
             static_cast<uint32_t>(data->overlapped_ptr.get()));

      return X_E_SUCCESS;
    }
    case 0x00021012: {
      uint32_t enabled = xe::load_and_swap<uint32_t>(buffer);
      XELOGD("XEnableGuestSignin: {}", enabled ? "true" : "false");
      return X_E_SUCCESS;
    }
    case 0x00022005: {
      struct XTITLE_GET_DEPLOYMENT_TYPE {
        xe::be<uint32_t> deployment_type_ptr;
        xe::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XTITLE_GET_DEPLOYMENT_TYPE*>(buffer);
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XTITLE_GET_DEPLOYMENT_TYPE));
      auto deployment_type =
          memory_->TranslateVirtual<uint32_t*>(data->deployment_type_ptr);
      *deployment_type = static_cast<uint32_t>(kernel_state_->deployment_type_);
      XELOGD("XTitleGetDeploymentType({:08X}, {:08X}",
             data->deployment_type_ptr.get(), data->overlapped_ptr.get());
      return X_E_SUCCESS;
    }
    // Causes dashboard to correctly process language/region change. It does not
    // contain any buffer.
    case 0x8000000D: {
      const bool is_pc_enabled =
          (kernel_state_->xconfig()->ReadSetting<uint8_t>(
               XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_FLAGS) &
           X_PC_FLAGS::PCEnabled) != 0;

      return is_pc_enabled ? X_E_ACCESS_DENIED : X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XAM message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
