// Copyright 2014 Intel Corporation. All rights reserved.
// Copyright 2017 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef OZONE_PLATFORM_OZONE_WAYLAND_WINDOW_H_
#define OZONE_PLATFORM_OZONE_WAYLAND_WINDOW_H_

#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "ozone/platform/channel_observer.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/events/platform/platform_event_dispatcher.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/location_hint.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/platform_window/platform_window.h"
#include "ui/platform_window/platform_window_init_properties.h"
#include "ui/views/widget/desktop_aura/neva/ui_constants.h"

class SkBitmap;

namespace IPC {
class Message;
}

namespace ui {

class BitmapCursor;
class OzoneGpuPlatformSupportHost;
class WindowGroupConfiguration;
class WindowManagerWayland;

class OzoneWaylandWindow : public PlatformWindow,
                           public PlatformEventDispatcher,
                           public ChannelObserver {
 public:
  OzoneWaylandWindow(PlatformWindowDelegate* delegate,
                     OzoneGpuPlatformSupportHost* sender,
                     WindowManagerWayland* window_manager,
                     const gfx::Rect& bounds,
                     ui::PlatformWindowOpacity opacity);
  OzoneWaylandWindow(const OzoneWaylandWindow&) = delete;
  OzoneWaylandWindow& operator=(const OzoneWaylandWindow&) = delete;
  ~OzoneWaylandWindow() override;

  unsigned GetHandle() const { return handle_; }
  PlatformWindowDelegate* GetDelegate() const { return delegate_; }

  // PlatformWindow:
  void InitPlatformWindow(neva::PlatformWindowType type,
                          gfx::AcceleratedWidget parent_window) override;
  void SetTitle(const std::u16string& title) override;
  void SetWindowShape(const SkPath& path) override;
  void SetOpacity(float opacity) override;
  void RequestDragData(const std::string& mime_type) override;
  void RequestSelectionData(const std::string& mime_type) override;
  void DragWillBeAccepted(uint32_t serial,
                          const std::string& mime_type) override;
  void DragWillBeRejected(uint32_t serial) override;
  gfx::Rect GetBoundsInPixels() const override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  void SetBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInDIP() const override;
  void Show(bool inactive) override;
  void Hide() override;
  void Close() override;
  bool IsVisible() const override;
  void PrepareForShutdown() override;
  void SetCapture() override;
  void ReleaseCapture() override;
  bool HasCapture() const override;
  void SetFullscreen(bool fullscreen, int64_t target_display_id) override;
  void SetFullscreenWithSize(bool fullscreen,
                             int64_t target_display_id,
                             const gfx::Size& size) override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  void SetResizeEnabled(bool enabled) override;
  bool GetResizeEnabled() const { return resize_enabled_; }
  PlatformWindowState GetPlatformWindowState() const override;

  void Activate() override;
  void Deactivate() override;

  void SetUseNativeFrame(bool use_native_frame) override;
  bool ShouldUseNativeFrame() const override;

  void SetCursor(scoped_refptr<PlatformCursor> cursor) override;
  void MoveCursorTo(const gfx::Point& location) override;
  void ConfineCursorToBounds(const gfx::Rect& bounds) override;
  void SetRestoredBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetRestoredBoundsInDIP() const override;
  void SetWindowIcons(const gfx::ImageSkia& window_icon,
                      const gfx::ImageSkia& app_icon) override;

  void SizeConstraintsChanged() override;
  void SetWindowProperty(const std::string& name,
                         const std::string& value) override;
  void SetLocationHint(gfx::LocationHint value) override;
  void ResetCustomCursor() override;
  void CreateGroup(const WindowGroupConfiguration&) override;
  void AttachToGroup(const std::string& group,
                     const std::string& layer) override;
  void FocusGroupOwner() override;
  void FocusGroupLayer() override;
  void DetachGroup() override;

  std::string GetDisplayId() override;

  void ShowInputPanel() override;
  void HideInputPanel(ImeHiddenType) override;
  void SetTextInputInfo(const ui::TextInputInfo& text_input_info) override;
  void SetSurroundingText(const std::string& text,
                          size_t cursor_position,
                          size_t anchor_position) override;
  void XInputActivate(const std::string& type) override;
  void XInputDeactivate() override;
  void XInputInvokeAction(uint32_t keysym,
                          ui::XInputKeySymbolType symbol_type,
                          ui::XInputEventType event_type) override;
  void SetCustomCursor(neva_app_runtime::CustomCursorType type,
                       const std::string& path,
                       int hotspot_x,
                       int hotspot_y,
                       bool allowed_cursor_overriding) override;
  void SetInputArea(const std::vector<gfx::Rect>& region) override;
  void SetCursorVisibility(bool visible) override;
  void SetGroupKeyMask(ui::KeyMask key_mask) override;
  void SetKeyMask(ui::KeyMask key_mask, bool set) override;

  // PlatformEventDispatcher:
  bool CanDispatchEvent(const PlatformEvent& event) override;
  uint32_t DispatchEvent(const PlatformEvent& event) override;

  // ChannelObserver:
  void OnGpuProcessLaunched() override;
  void OnChannelDestroyed() override;

 private:
  void DeferredSendingToGpu();
  void SendWidgetState();
  void AddRegion();
  void ResetRegion();
  void SetCursor();
  void ValidateBounds();
  void SetCustomCursorFromBitmap(neva_app_runtime::CustomCursorType type,
                                 SkBitmap* cursor_image,
                                 int hotspot_x,
                                 int hotspot_y,
                                 bool allowed_cursor_overriding);
  void SetContentsBounds();

  PlatformWindowDelegate* delegate_;   // Not owned.
  OzoneGpuPlatformSupportHost* sender_;  // Not owned.
  WindowManagerWayland* window_manager_;  // Not owned.
  bool transparent_;
  gfx::Rect bounds_;
  ui::PlatformWindowOpacity opacity_;
  bool resize_enabled_ = true;
  unsigned handle_;
  unsigned parent_;
  ui::WidgetType type_;
  ui::WidgetState state_;
  SkRegion* region_;
  std::u16string title_;
  // The current cursor bitmap (immutable).
  scoped_refptr<BitmapCursor> bitmap_;
  std::string display_id_ = "-1";
  bool init_window_;
  neva_app_runtime::CustomCursorType cursor_type_ =
      neva_app_runtime::CustomCursorType::kNotUse;
  bool allowed_cursor_overriding_ = false;
  std::queue<std::unique_ptr<IPC::Message>> deferred_messages_;
  base::WeakPtrFactory<OzoneWaylandWindow> weak_factory_;
};

}  // namespace ui

#endif  // OZONE_PLATFORM_OZONE_WAYLAND_WINDOW_H_
