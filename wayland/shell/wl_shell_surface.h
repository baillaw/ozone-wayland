// Copyright 2014 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OZONE_WAYLAND_SHELL_WL_SHELL_SURFACE_H_
#define OZONE_WAYLAND_SHELL_WL_SHELL_SURFACE_H_

#include "ozone/wayland/shell/shell_surface.h"

namespace ozonewayland {

class WaylandSurface;
class WaylandWindow;

class WLShellSurface : public WaylandShellSurface {
 public:
  WLShellSurface();
  virtual ~WLShellSurface();

  virtual void InitializeShellSurface(WaylandWindow* window,
                                      WaylandWindow::ShellType type) OVERRIDE;
  virtual void UpdateShellSurface(WaylandWindow::ShellType type,
                                  WaylandShellSurface* shell_parent,
                                  unsigned x,
                                  unsigned y) OVERRIDE;
  virtual void SetWindowTitle(const base::string16& title) OVERRIDE;
  virtual void Maximize() OVERRIDE;
  virtual void Minimize() OVERRIDE;
  virtual void Unminimize() OVERRIDE;
  virtual bool IsMinimized() const OVERRIDE;

  static void HandleConfigure(void* data,
                              struct wl_shell_surface* shell_surface,
                              uint32_t edges,
                              int32_t width,
                              int32_t height);
  static void HandlePopupDone(void* data,
                              struct wl_shell_surface* shell_surface);
  static void HandlePing(void* data,
                         struct wl_shell_surface* shell_surface,
                         uint32_t serial);

 private:
  wl_shell_surface* shell_surface_;
  DISALLOW_COPY_AND_ASSIGN(WLShellSurface);
};

}  // namespace ozonewayland

#endif  // OZONE_WAYLAND_SHELL_WL_SHELL_SURFACE_H_
