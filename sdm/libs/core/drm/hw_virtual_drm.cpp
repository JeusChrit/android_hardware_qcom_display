/*
Copyright (c) 2017, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <ctype.h>
#include <drm_logger.h>
#include <utils/debug.h>
#include <algorithm>
#include <vector>
#include "hw_device_drm.h"
#include "hw_virtual_drm.h"
#include "hw_info_drm.h"

#define __CLASS__ "HWVirtualDRM"

using sde_drm::DRMDisplayType;
using sde_drm::DRMConnectorInfo;
using sde_drm::DRMRect;
using sde_drm::DRMOps;
using sde_drm::DRMPowerMode;
using sde_drm::DRMSecureMode;
namespace sdm {

HWVirtualDRM::HWVirtualDRM(BufferSyncHandler *buffer_sync_handler,
                           BufferAllocator *buffer_allocator,
                           HWInfoInterface *hw_info_intf)
                           : HWDeviceDRM(buffer_sync_handler, buffer_allocator, hw_info_intf) {
  HWDeviceDRM::device_name_ = "Virtual Display Device";
  HWDeviceDRM::disp_type_ = DRMDisplayType::VIRTUAL;
}

DisplayError HWVirtualDRM::Init() {
  DisplayError err = HWDeviceDRM::Init();
  if (err != kErrorNone) {
    return err;
  }
  // TODO(user): Remove this code once driver populates appropriate topology based on virtual
  // display configuration
  if (connector_info_.topology == sde_drm::DRMTopology::UNKNOWN) {
    uint32_t max_width = 0;
    for (uint32_t i = 0; i < (uint32_t)connector_info_.modes.size(); i++) {
      max_width = std::max(max_width, UINT32(connector_info_.modes[i].hdisplay));
    }
    connector_info_.topology = sde_drm::DRMTopology::SINGLE_LM;
    if (max_width > hw_resource_.max_mixer_width) {
      connector_info_.topology = sde_drm::DRMTopology::DUAL_LM_MERGE;
    }
  }
  InitializeConfigs();

  return kErrorNone;
}

void HWVirtualDRM::ConfigureWbConnectorFbId(uint32_t fb_id) {
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_OUTPUT_FB_ID, token_.conn_id, fb_id);
  return;
}

void HWVirtualDRM::ConfigureWbConnectorDestRect() {
  DRMRect dst = {};
  dst.left = 0;
  dst.bottom = display_attributes_[current_mode_index_].y_pixels;
  dst.top = 0;
  dst.right = display_attributes_[current_mode_index_].x_pixels;
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_OUTPUT_RECT, token_.conn_id, dst);
  return;
}

void HWVirtualDRM::ConfigureWbConnectorSecureMode(bool secure) {
  DRMSecureMode secure_mode = secure ? DRMSecureMode::SECURE : DRMSecureMode::NON_SECURE;
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_FB_SECURE_MODE, token_.conn_id, secure_mode);
}

void HWVirtualDRM::InitializeConfigs() {
  display_attributes_.resize(connector_info_.modes.size());
  for (uint32_t i = 0; i < connector_info_.modes.size(); i++) {
    PopulateDisplayAttributes(i);
  }
}

DisplayError HWVirtualDRM::SetWbConfigs(const HWDisplayAttributes &display_attributes) {
  int ret = -EINVAL;
  int mode_index = -1;
  // Add new connector mode to the list
  drmModeModeInfo mode = {};
  mode.hdisplay = mode.hsync_start = mode.hsync_end = mode.htotal =
                                       UINT16(display_attributes.x_pixels);
  mode.vdisplay = mode.vsync_start = mode.vsync_end = mode.vtotal =
                                       UINT16(display_attributes.y_pixels);
  mode.vrefresh = UINT32(display_attributes.fps);
  mode.clock = (mode.htotal * mode.vtotal * mode.vrefresh) / 1000;
  connector_info_.modes.push_back(mode);

  // Inform the updated mode list to the driver
  struct sde_drm_wb_cfg wb_cfg = {};
  wb_cfg.connector_id = token_.conn_id;
  wb_cfg.flags = SDE_DRM_WB_CFG_FLAGS_CONNECTED;
  wb_cfg.count_modes = UINT32(connector_info_.modes.size());
  wb_cfg.modes = (uint64_t)connector_info_.modes.data();
#ifdef DRM_IOCTL_SDE_WB_CONFIG
  ret = drmIoctl(dev_fd_, DRM_IOCTL_SDE_WB_CONFIG, &wb_cfg);
#endif
  if (ret) {
    DLOGE("Dump WBConfig: mode_count %d flags %x", wb_cfg.count_modes, wb_cfg.flags);
    DumpConnectorModeInfo();
    return kErrorHardware;
  }
  // Reload connector info for updated info after null commit
  drm_mgr_intf_->GetConnectorInfo(token_.conn_id, &connector_info_);
  InitializeConfigs();

  GetModeIndex(display_attributes, &mode_index);
  if (mode_index < 0) {
    DLOGE("Mode not found for resolution %dx%d fps %d", display_attributes.x_pixels,
          display_attributes.y_pixels, UINT32(display_attributes.fps));
    DumpConnectorModeInfo();
    return kErrorNotSupported;
  }
  current_mode_index_ = UINT32(mode_index);

  DumpConnectorModeInfo();

  return kErrorNone;
}

void HWVirtualDRM::DumpConnectorModeInfo() {
  for (uint32_t i = 0; i < (uint32_t)connector_info_.modes.size(); i++) {
    DLOGI("Mode[%d]: Name: %s\tvref: %d\thdisp: %d\t hsync_s: %d\thsync_e:%d\thtotal: %d\t" \
          "vdisp: %d\tvsync_s: %d\tvsync_e: %d\tvtotal: %d\n", i, connector_info_.modes[i].name,
          connector_info_.modes[i].vrefresh, connector_info_.modes[i].hdisplay,
          connector_info_.modes[i].hsync_start, connector_info_.modes[i].hsync_end,
          connector_info_.modes[i].htotal, connector_info_.modes[i].vdisplay,
          connector_info_.modes[i].vsync_start, connector_info_.modes[i].vsync_end,
          connector_info_.modes[i].vtotal);
  }
}

DisplayError HWVirtualDRM::Commit(HWLayers *hw_layers) {
  LayerBuffer *output_buffer = hw_layers->info.stack->output_buffer;
  DisplayError err = kErrorNone;

  if (first_cycle_) {
    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_CRTC, token_.conn_id, token_.crtc_id);
    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_POWER_MODE, token_.conn_id, DRMPowerMode::ON);
    first_cycle_ = false;
  }

  registry_.RegisterCurrent(hw_layers);
  registry_.MapBufferToFbId(output_buffer);
  uint32_t fb_id = registry_.GetFbId(output_buffer->planes[0].fd);

  ConfigureWbConnectorFbId(fb_id);
  ConfigureWbConnectorDestRect();
  ConfigureWbConnectorSecureMode(output_buffer->flags.secure);

  err = HWDeviceDRM::AtomicCommit(hw_layers);
  if (err != kErrorNone) {
    DLOGE("Atomic commit failed for crtc_id %d conn_id %d", token_.crtc_id, token_.conn_id);
  }
  registry_.UnregisterNext();

  return(err);
}

DisplayError HWVirtualDRM::Validate(HWLayers *hw_layers) {
  LayerBuffer *output_buffer = hw_layers->info.stack->output_buffer;

  if (first_cycle_) {
    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_CRTC, token_.conn_id, token_.crtc_id);
    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_POWER_MODE, token_.conn_id, DRMPowerMode::ON);
  }

  registry_.MapBufferToFbId(output_buffer);
  uint32_t fb_id = registry_.GetFbId(output_buffer->planes[0].fd);

  ConfigureWbConnectorFbId(fb_id);
  ConfigureWbConnectorDestRect();
  ConfigureWbConnectorSecureMode(output_buffer->flags.secure);

  return HWDeviceDRM::Validate(hw_layers);
}

DisplayError HWVirtualDRM::SetDisplayAttributes(const HWDisplayAttributes &display_attributes) {
  if (display_attributes.x_pixels == 0 || display_attributes.y_pixels == 0) {
    return kErrorParameters;
  }

  int mode_index = -1;
  GetModeIndex(display_attributes, &mode_index);
  if (mode_index < 0) {
    DisplayError error = SetWbConfigs(display_attributes);
    if (error != kErrorNone) {
      return error;
    }
  } else {
    current_mode_index_ = UINT32(mode_index);
  }
  PopulateHWPanelInfo();
  UpdateMixerAttributes();

  DLOGI("New WB Resolution: %dx%d cur_mode_index %d", display_attributes.x_pixels,
        display_attributes.y_pixels, current_mode_index_);
  return kErrorNone;
}

DisplayError HWVirtualDRM::PowerOn() {
  if (first_cycle_) {
    return kErrorNone;
  }

  return HWDeviceDRM::PowerOn();
}

DisplayError HWVirtualDRM::GetPPFeaturesVersion(PPFeatureVersion *vers) {
  return kErrorNone;
}

void HWVirtualDRM::GetModeIndex(const HWDisplayAttributes &display_attributes, int *mode_index) {
  *mode_index = -1;
  for (uint32_t i = 0; i < connector_info_.modes.size(); i++) {
    if (display_attributes.x_pixels == connector_info_.modes[i].hdisplay &&
        display_attributes.y_pixels == connector_info_.modes[i].vdisplay &&
        display_attributes.fps == connector_info_.modes[i].vrefresh) {
      *mode_index = INT32(i);
      break;
    }
  }
}

}  // namespace sdm
