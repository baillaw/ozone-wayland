// Copyright 2014 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OZONE_IMPL_DESKTOP_AURA_WINDOW_TREE_HOST_DELEGATE_WAYLAND_H_
#define OZONE_IMPL_DESKTOP_AURA_WINDOW_TREE_HOST_DELEGATE_WAYLAND_H_

#include <list>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/message_loop/message_loop.h"
#include "ozone/ui/events/window_change_observer.h"
#include "ui/events/event.h"
#include "ui/events/event_source.h"
#include "ui/events/platform/platform_event_dispatcher.h"
#include "ui/gfx/native_widget_types.h"

namespace ui {
class OzoneWaylandWindow;
}

namespace views {

class DesktopWindowTreeHostWayland;

// A static class used by DesktopWindowTreeHostWayland to dispatch native events
// and basic window management.
class WindowTreeHostDelegateWayland
    : public ui::WindowChangeObserver {
 public:
  WindowTreeHostDelegateWayland();
  virtual ~WindowTreeHostDelegateWayland();

  void OnRootWindowCreated(ui::OzoneWaylandWindow* window);
  void OnRootWindowClosed(ui::OzoneWaylandWindow* window);

  ui::OzoneWaylandWindow* GetWindow(unsigned handle);
  bool HasWindowsOpen() const;

 private:

  // Window Change Observer.
  virtual void OnWindowFocused(unsigned handle) OVERRIDE;
  virtual void OnWindowEnter(unsigned handle) OVERRIDE;
  virtual void OnWindowLeave(unsigned handle) OVERRIDE;
  virtual void OnWindowClose(unsigned handle) OVERRIDE;
  virtual void OnWindowResized(unsigned windowhandle,
                               unsigned width,
                               unsigned height) OVERRIDE;
  virtual void OnWindowUnminimized(unsigned windowhandle) OVERRIDE;
  virtual void OnWindowDeActivated(unsigned windowhandle) OVERRIDE;
  virtual void OnWindowActivated(unsigned windowhandle) OVERRIDE;

  // Dispatches a mouse event.
  std::list<ui::OzoneWaylandWindow*>& open_windows();

  // List of all open aura::Window.
  std::list<ui::OzoneWaylandWindow*>* open_windows_;
  DISALLOW_COPY_AND_ASSIGN(WindowTreeHostDelegateWayland);
};

}  // namespace views

#endif  // OZONE_IMPL_DESKTOP_AURA_WINDOW_TREE_HOST_DELEGATE_WAYLAND_H_
