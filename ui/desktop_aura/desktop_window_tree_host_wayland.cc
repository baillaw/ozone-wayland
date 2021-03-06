// Copyright 2013 The Chromium Authors. All rights reserved.
// Copyright 2013 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ozone/ui/desktop_aura/desktop_window_tree_host_wayland.h"

#include <string>

#include "base/bind.h"
#include "ozone/ui/desktop_aura/desktop_drag_drop_client_wayland.h"
#include "ozone/ui/desktop_aura/desktop_screen_wayland.h"
#include "ozone/ui/events/window_state_change_handler.h"
#include "ui/aura/client/cursor_client.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/window_property.h"
#include "ui/base/dragdrop/os_exchange_data_provider_aura.h"
#include "ui/base/hit_test.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/input_method_auralinux.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/events/event_utils.h"
#include "ui/gfx/insets.h"
#include "ui/native_theme/native_theme.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/surface_factory_ozone.h"
#include "ui/platform_window/platform_window.h"
#include "ui/platform_window/platform_window_delegate.h"
#include "ui/views/corewm/tooltip_aura.h"
#include "ui/views/ime/input_method.h"
#include "ui/views/linux_ui/linux_ui.h"
#include "ui/views/views_export.h"
#include "ui/views/views_delegate.h"
#include "ui/views/widget/desktop_aura/desktop_dispatcher_client.h"
#include "ui/views/widget/desktop_aura/desktop_native_cursor_manager.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/desktop_aura/desktop_screen_position_client.h"
#include "ui/wm/core/input_method_event_filter.h"
#include "ui/wm/core/window_util.h"
#include "ui/wm/public/window_move_client.h"


namespace views {

DesktopWindowTreeHostWayland*
    DesktopWindowTreeHostWayland::g_current_capture = NULL;

DesktopWindowTreeHostWayland*
    DesktopWindowTreeHostWayland::g_current_dispatcher = NULL;

DesktopWindowTreeHostWayland*
    DesktopWindowTreeHostWayland::g_active_window = NULL;

std::list<gfx::AcceleratedWidget>*
DesktopWindowTreeHostWayland::open_windows_ = NULL;

std::vector<aura::Window*>*
DesktopWindowTreeHostWayland::aura_windows_ = NULL;

DEFINE_WINDOW_PROPERTY_KEY(
    aura::Window*, kViewsWindowForRootWindow, NULL);

DEFINE_WINDOW_PROPERTY_KEY(
    DesktopWindowTreeHostWayland*, kHostForRootWindow, NULL);

DesktopWindowTreeHostWayland::DesktopWindowTreeHostWayland(
    internal::NativeWidgetDelegate* native_widget_delegate,
    DesktopNativeWidgetAura* desktop_native_widget_aura)
    : aura::WindowTreeHost(),
      state_(Uninitialized),
      previous_bounds_(0, 0, 0, 0),
      previous_maximize_bounds_(0, 0, 0, 0),
      window_(0),
      title_(base::string16()),
      close_widget_factory_(this),
      drag_drop_client_(NULL),
      native_widget_delegate_(native_widget_delegate),
      content_window_(NULL),
      desktop_native_widget_aura_(desktop_native_widget_aura),
      window_parent_(NULL),
      window_children_() {
}

DesktopWindowTreeHostWayland::~DesktopWindowTreeHostWayland() {
  window()->ClearProperty(kHostForRootWindow);
  aura::client::SetWindowMoveClient(window(), NULL);
  desktop_native_widget_aura_->OnDesktopWindowTreeHostDestroyed(this);
  DestroyDispatcher();
}

// static
DesktopWindowTreeHostWayland*
DesktopWindowTreeHostWayland::GetHostForAcceleratedWidget(
    gfx::AcceleratedWidget widget) {
  aura::WindowTreeHost* host =
      aura::WindowTreeHost::GetForAcceleratedWidget(widget);

  return host ? host->window()->GetProperty(kHostForRootWindow) : NULL;
}

// static
const std::vector<aura::Window*>&
DesktopWindowTreeHostWayland::GetAllOpenWindows() {
  if (!aura_windows_) {
    const std::list<gfx::AcceleratedWidget>& windows = open_windows();
    aura_windows_ = new std::vector<aura::Window*>(windows.size());
    std::transform(
        windows.begin(), windows.end(), aura_windows_->begin(),
            DesktopWindowTreeHostWayland::GetContentWindowForAcceleratedWidget);
    }

  return *aura_windows_;
}

void DesktopWindowTreeHostWayland::CleanUpWindowList() {
  delete open_windows_;
  open_windows_ = NULL;
  if (aura_windows_) {
    aura_windows_->clear();
    delete aura_windows_;
    aura_windows_ = NULL;
  }
}

// static
aura::Window*
DesktopWindowTreeHostWayland::GetContentWindowForAcceleratedWidget(
    gfx::AcceleratedWidget widget) {
  aura::WindowTreeHost* host =
      aura::WindowTreeHost::GetForAcceleratedWidget(widget);

  return host ?
      host->window()->GetProperty(kViewsWindowForRootWindow) : NULL;
}

gfx::Rect DesktopWindowTreeHostWayland::GetBoundsInScreen() const {
  return platform_window_->GetBounds();
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWayland, private:

void DesktopWindowTreeHostWayland::InitWaylandWindow(
    const Widget::InitParams& params) {
  const gfx::Rect& bounds = gfx::Rect(params.bounds.origin(),
                                      AdjustSize(params.bounds.size()));
  platform_window_ =
      ui::OzonePlatform::GetInstance()->CreatePlatformWindow(this, bounds);
  DCHECK(window_);
  // Maintain parent child relation as done in X11 version.
  // If we have a parent, record the parent/child relationship. We use this
  // data during destruction to make sure that when we try to close a parent
  // window, we also destroy all child windows.
  gfx::AcceleratedWidget parent_window = 0;
  if (params.parent && params.parent->GetHost()) {
    parent_window = params.parent->GetHost()->GetAcceleratedWidget();
    window_parent_ = GetHostForAcceleratedWidget(parent_window);
    DCHECK(window_parent_);
    window_parent_->window_children_.insert(this);
  }

  ui::PlatformWindow::PlatformWindowType type =
      ui::PlatformWindow::PLATFORM_WINDOW_UNKNOWN;
  switch (params.type) {
    case Widget::InitParams::TYPE_TOOLTIP: {
      type = ui::PlatformWindow::PLATFORM_WINDOW_TYPE_TOOLTIP;
      break;
    }
    case Widget::InitParams::TYPE_POPUP: {
      type = ui::PlatformWindow::PLATFORM_WINDOW_TYPE_POPUP;
      break;
    }
    case Widget::InitParams::TYPE_MENU: {
      type = ui::PlatformWindow::PLATFORM_WINDOW_TYPE_MENU;
      break;
    }
    case Widget::InitParams::TYPE_BUBBLE: {
      type = ui::PlatformWindow::PLATFORM_WINDOW_TYPE_BUBBLE;
      break;
    }
    case Widget::InitParams::TYPE_WINDOW: {
      type = ui::PlatformWindow::PLATFORM_WINDOW_TYPE_WINDOW;
      break;
    }
    case Widget::InitParams::TYPE_WINDOW_FRAMELESS: {
      type = ui::PlatformWindow::PLATFORM_WINDOW_TYPE_WINDOW_FRAMELESS;
      break;
    }
    default:
      break;
  }

  if (!parent_window && g_active_window)
    parent_window = g_active_window->window_;

  platform_window_->InitPlatformWindow(type, parent_window);
  // If we have a delegate which is providing a default window icon, use that
  // icon.
  gfx::ImageSkia* window_icon = ViewsDelegate::views_delegate ?
      ViewsDelegate::views_delegate->GetDefaultWindowIcon() : NULL;
  if (window_icon) {
    SetWindowIcons(gfx::ImageSkia(), *window_icon);
  }

  CreateCompositor(GetAcceleratedWidget());
  if (ui::PlatformEventSource::GetInstance())
    ui::PlatformEventSource::GetInstance()->AddPlatformEventDispatcher(this);
}

void DesktopWindowTreeHostWayland::OnAcceleratedWidgetAvailable(
      gfx::AcceleratedWidget widget) {
  window_ = widget;
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWayland, DesktopWindowTreeHost implementation:

void DesktopWindowTreeHostWayland::Init(
    aura::Window* content_window,
    const Widget::InitParams& params) {
  content_window_ = content_window;
  // In some situations, views tries to make a zero sized window, and that
  // makes us crash. Make sure we have valid sizes.
  Widget::InitParams sanitized_params = params;
  if (sanitized_params.bounds.width() == 0)
    sanitized_params.bounds.set_width(100);
  if (sanitized_params.bounds.height() == 0)
    sanitized_params.bounds.set_height(100);

  InitWaylandWindow(sanitized_params);
}

void DesktopWindowTreeHostWayland::OnNativeWidgetCreated(
    const Widget::InitParams& params) {
  window()->SetProperty(kViewsWindowForRootWindow, content_window_);
  window()->SetProperty(kHostForRootWindow, this);

  // If we're given a parent, we need to mark ourselves as transient to another
  // window. Otherwise activation gets screwy.
  gfx::NativeView parent = params.parent;
  if (!params.child && params.parent)
    wm::AddTransientChild(parent, content_window_);

  native_widget_delegate_->OnNativeWidgetCreated(true);
  open_windows().push_back(window_);
  if (aura_windows_) {
    aura_windows_->clear();
    delete aura_windows_;
    aura_windows_ = NULL;
  }
}

scoped_ptr<corewm::Tooltip>
DesktopWindowTreeHostWayland::CreateTooltip() {
  return scoped_ptr<corewm::Tooltip>(
             new corewm::TooltipAura(gfx::SCREEN_TYPE_NATIVE));
}

scoped_ptr<aura::client::DragDropClient>
DesktopWindowTreeHostWayland::CreateDragDropClient(
    DesktopNativeCursorManager* cursor_manager) {
  drag_drop_client_ = new DesktopDragDropClientWayland(window());
  return scoped_ptr<aura::client::DragDropClient>(drag_drop_client_).Pass();
}

void DesktopWindowTreeHostWayland::Close() {
  if (!close_widget_factory_.HasWeakPtrs()) {
    // And we delay the close so that if we are called from an ATL callback,
    // we don't destroy the window before the callback returned (as the caller
    // may delete ourselves on destroy and the ATL callback would still
    // dereference us when the callback returns).
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(&DesktopWindowTreeHostWayland::CloseNow,
                   close_widget_factory_.GetWeakPtr()));
  }
}

void DesktopWindowTreeHostWayland::CloseNow() {
  if (!window_)
    return;

  unsigned widgetId = window_;
  ReleaseCapture();
  native_widget_delegate_->OnNativeWidgetDestroying();

  // If we have children, close them. Use a copy for iteration because they'll
  // remove themselves.
  std::set<DesktopWindowTreeHostWayland*> window_children_copy =
      window_children_;
  for (std::set<DesktopWindowTreeHostWayland*>::iterator it =
           window_children_copy.begin(); it != window_children_copy.end();
       ++it) {
    (*it)->CloseNow();
  }
  DCHECK(window_children_.empty());

  // If we have a parent, remove ourselves from its children list.
  if (window_parent_) {
    window_parent_->window_children_.erase(this);
    window_parent_ = NULL;
  }

  // Destroy the compositor before destroying the window since shutdown
  // may try to swap, and the swap without a window causes an error, which
  // causes a crash with in-process renderer.
  DestroyCompositor();

  open_windows().remove(widgetId);
  if (aura_windows_) {
    aura_windows_->clear();
    delete aura_windows_;
    aura_windows_ = NULL;
  }

  if (g_active_window == this)
    g_active_window = NULL;

  if (g_current_dispatcher == this)
    g_current_dispatcher = NULL;

  // Actually free our native resources.
  platform_window_->Close();
  window_ = 0;
  if (open_windows().empty())
    CleanUpWindowList();

  ui::PlatformEventSource* event_source =
      ui::PlatformEventSource::GetInstance();
  if (event_source)
    event_source->RemovePlatformEventDispatcher(this);

  desktop_native_widget_aura_->OnHostClosed();
}

aura::WindowTreeHost* DesktopWindowTreeHostWayland::AsWindowTreeHost() {
  return this;
}

void DesktopWindowTreeHostWayland::ShowWindowWithState(
    ui::WindowShowState show_state) {
  if (show_state == ui::SHOW_STATE_NORMAL ||
      show_state == ui::SHOW_STATE_MAXIMIZED) {
    Activate();
  }

  state_ |= Visible;
  native_widget_delegate_->AsWidget()->SetInitialFocus(show_state);
}

void DesktopWindowTreeHostWayland::ShowMaximizedWithBounds(
    const gfx::Rect& restored_bounds) {
  Maximize();
  previous_bounds_ = restored_bounds;
  Show();
}

bool DesktopWindowTreeHostWayland::IsVisible() const {
  return state_ & Visible;
}

void DesktopWindowTreeHostWayland::SetSize(const gfx::Size& requested_size) {
  gfx::Size size = AdjustSize(requested_size);
  gfx::Rect new_bounds = platform_window_->GetBounds();
  new_bounds.set_size(size);
  platform_window_->SetBounds(new_bounds);
}

void DesktopWindowTreeHostWayland::StackAtTop() {
}

void DesktopWindowTreeHostWayland::CenterWindow(const gfx::Size& size) {
  gfx::Rect parent_bounds = GetWorkAreaBoundsInScreen();

  // If |window_|'s transient parent bounds are big enough to contain |size|,
  // use them instead.
  if (wm::GetTransientParent(content_window_)) {
    gfx::Rect transient_parent_rect =
        wm::GetTransientParent(content_window_)->GetBoundsInScreen();
    if (transient_parent_rect.height() >= size.height() &&
      transient_parent_rect.width() >= size.width()) {
      parent_bounds = transient_parent_rect;
    }
  }

  gfx::Rect window_bounds(
      parent_bounds.x() + (parent_bounds.width() - size.width()) / 2,
      parent_bounds.y() + (parent_bounds.height() - size.height()) / 2,
      size.width(),
      size.height());
  // Don't size the window bigger than the parent, otherwise the user may not be
  // able to close or move it.
  window_bounds.AdjustToFit(parent_bounds);
  platform_window_->SetBounds(window_bounds);
}

void DesktopWindowTreeHostWayland::GetWindowPlacement(
    gfx::Rect* bounds,
    ui::WindowShowState* show_state) const {
  *bounds = GetRestoredBounds();

  if (IsMinimized()) {
    *show_state = ui::SHOW_STATE_MINIMIZED;
  } else if (IsFullscreen()) {
    *show_state = ui::SHOW_STATE_FULLSCREEN;
  } else if (IsMaximized()) {
    *show_state = ui::SHOW_STATE_MAXIMIZED;
  } else if (!IsActive()) {
    *show_state = ui::SHOW_STATE_INACTIVE;
  } else {
    *show_state = ui::SHOW_STATE_NORMAL;
  }
}

gfx::Rect DesktopWindowTreeHostWayland::GetWindowBoundsInScreen() const {
  return platform_window_->GetBounds();
}

gfx::Rect DesktopWindowTreeHostWayland::GetClientAreaBoundsInScreen() const {
  // TODO(erg): The NativeWidgetAura version returns |bounds_|, claiming its
  // needed for View::ConvertPointToScreen() to work
  // correctly. DesktopWindowTreeHostWin::GetClientAreaBoundsInScreen() just
  // asks windows what it thinks the client rect is.
  //
  // Attempts to calculate the rect by asking the NonClientFrameView what it
  // thought its GetBoundsForClientView() were broke combobox drop down
  // placement.
  return platform_window_->GetBounds();
}

gfx::Rect DesktopWindowTreeHostWayland::GetRestoredBounds() const {
  if (!previous_bounds_.IsEmpty())
    return previous_bounds_;

  return GetWindowBoundsInScreen();
}

gfx::Rect DesktopWindowTreeHostWayland::GetWorkAreaBoundsInScreen() const {
  // TODO(kalyan): Take into account wm decorations. i.e Dock, panel etc.
  gfx::Screen *screen = gfx::Screen::GetScreenByType(gfx::SCREEN_TYPE_NATIVE);
  if (!screen)
    NOTREACHED() << "Unable to retrieve valid gfx::Screen";

  gfx::Display display = screen->GetPrimaryDisplay();
  return display.bounds();
}

void DesktopWindowTreeHostWayland::SetShape(gfx::NativeRegion native_region) {
  // TODO(erg):
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::Activate() {
  if (state_ & Active)
    return;

  state_ |= Active;
  if (state_ & Visible) {
    OnActivationChanged(true);
  }
}

void DesktopWindowTreeHostWayland::Deactivate() {
  if (!(state_ & Active))
    return;

  state_ &= ~Active;
  ReleaseCapture();
  OnActivationChanged(false);
}

bool DesktopWindowTreeHostWayland::IsActive() const {
  return state_ & Active;
}

void DesktopWindowTreeHostWayland::Maximize() {
  if (state_ & Maximized)
    return;

  state_ |= Maximized;
  state_ &= ~Minimized;
  state_ &= ~Normal;
  previous_bounds_ = platform_window_->GetBounds();
  platform_window_->Maximize();
  if (IsMinimized())
    ShowWindowWithState(ui::SHOW_STATE_NORMAL);
  Relayout();
}

void DesktopWindowTreeHostWayland::Minimize() {
  if (state_ & Minimized)
    return;

  state_ |= Minimized;
  previous_bounds_ = platform_window_->GetBounds();
  ReleaseCapture();
  compositor()->SetVisible(false);
  content_window_->Hide();
  platform_window_->Minimize();
  Relayout();
}

void DesktopWindowTreeHostWayland::Restore() {
  if (state_ & Normal)
    return;

  state_ &= ~Maximized;
  if (state_ & Minimized) {
    content_window_->Show();
    compositor()->SetVisible(true);
  }

  state_ &= ~Minimized;
  state_ |= Normal;
  platform_window_->Restore();
  platform_window_->SetBounds(previous_bounds_);
  previous_bounds_ = gfx::Rect();
  Relayout();
  if (IsMinimized())
    ShowWindowWithState(ui::SHOW_STATE_NORMAL);
}

bool DesktopWindowTreeHostWayland::IsMaximized() const {
  return state_ & Maximized;
}

bool DesktopWindowTreeHostWayland::IsMinimized() const {
  return state_ & Minimized;
}

bool DesktopWindowTreeHostWayland::HasCapture() const {
  return g_current_capture == this;
}

bool DesktopWindowTreeHostWayland::IsAlwaysOnTop() const {
  NOTIMPLEMENTED();
  return false;
}

void DesktopWindowTreeHostWayland::SetVisibleOnAllWorkspaces(
    bool always_visible) {
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::SizeConstraintsChanged() {
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::SetAlwaysOnTop(bool always_on_top) {
  // TODO(erg):
  NOTIMPLEMENTED();
}

bool DesktopWindowTreeHostWayland::SetWindowTitle(const base::string16& title) {
  if (title.compare(title_)) {
    ui::WindowStateChangeHandler::GetInstance()->SetWidgetTitle(window_, title);
    title_ = title;
    return true;
  }

  return false;
}

void DesktopWindowTreeHostWayland::ClearNativeFocus() {
  // This method is weird and misnamed. Instead of clearing the native focus,
  // it sets the focus to our |content_window_|, which will trigger a cascade
  // of focus changes into views.
  if (content_window_ && aura::client::GetFocusClient(content_window_) &&
      content_window_->Contains(
          aura::client::GetFocusClient(content_window_)->GetFocusedWindow())) {
    aura::client::GetFocusClient(content_window_)->FocusWindow(content_window_);
  }
}

Widget::MoveLoopResult DesktopWindowTreeHostWayland::RunMoveLoop(
    const gfx::Vector2d& drag_offset,
    Widget::MoveLoopSource source,
    Widget::MoveLoopEscapeBehavior escape_behavior) {
  NOTIMPLEMENTED();
  return Widget::MOVE_LOOP_SUCCESSFUL;
}

void DesktopWindowTreeHostWayland::EndMoveLoop() {
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::SetVisibilityChangedAnimationsEnabled(
    bool value) {
  // Much like the previous NativeWidgetGtk, we don't have anything to do here.
}

bool DesktopWindowTreeHostWayland::ShouldUseNativeFrame() const {
  return false;
}

bool DesktopWindowTreeHostWayland::ShouldWindowContentsBeTransparent() const {
  return false;
}

void DesktopWindowTreeHostWayland::FrameTypeChanged() {
  Widget::FrameType new_type =
    native_widget_delegate_->AsWidget()->frame_type();
  if (new_type == Widget::FRAME_TYPE_DEFAULT) {
    // The default is determined by Widget::InitParams::remove_standard_frame
    // and does not change.
    return;
  }

  // Replace the frame and layout the contents. Even though we don't have a
  // swapable glass frame like on Windows, we still replace the frame because
  // the button assets don't update otherwise.
  native_widget_delegate_->AsWidget()->non_client_view()->UpdateFrame();
}

void DesktopWindowTreeHostWayland::SetFullscreen(bool fullscreen) {
  if ((state_ & FullScreen) == fullscreen)
    return;

  if (fullscreen) {
    state_ |= FullScreen;
    state_ &= ~Normal;
  } else {
    state_ &= ~FullScreen;
  }

  if (!(state_ & FullScreen)) {
    if (state_ & Maximized) {
      previous_bounds_ = previous_maximize_bounds_;
      previous_maximize_bounds_ = gfx::Rect();
      platform_window_->Maximize();
    } else {
      Restore();
    }
  } else {
    if (state_ & Maximized)
      previous_maximize_bounds_ = previous_bounds_;

    previous_bounds_ = platform_window_->GetBounds();
    platform_window_->ToggleFullscreen();
  }

  Relayout();
}

bool DesktopWindowTreeHostWayland::IsFullscreen() const {
  return state_ & FullScreen;
}

void DesktopWindowTreeHostWayland::SetOpacity(unsigned char opacity) {
  // TODO(erg):
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::SetWindowIcons(
    const gfx::ImageSkia& window_icon, const gfx::ImageSkia& app_icon) {
  // TODO(erg):
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::InitModalType(ui::ModalType modal_type) {
  switch (modal_type) {
    case ui::MODAL_TYPE_NONE:
      break;
    default:
      // TODO(erg): Figure out under what situations |modal_type| isn't
      // none. The comment in desktop_native_widget_aura.cc suggests that this
      // is rare.
      NOTIMPLEMENTED();
  }
}

void DesktopWindowTreeHostWayland::FlashFrame(bool flash_frame) {
  // TODO(erg):
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::OnRootViewLayout() {
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::OnNativeWidgetFocus() {
  native_widget_delegate_->AsWidget()->GetInputMethod()->OnFocus();
}

void DesktopWindowTreeHostWayland::OnNativeWidgetBlur() {
  if (window_)
    native_widget_delegate_->AsWidget()->GetInputMethod()->OnBlur();
}

bool DesktopWindowTreeHostWayland::IsAnimatingClosed() const {
  return false;
}

bool DesktopWindowTreeHostWayland::IsTranslucentWindowOpacitySupported() const {
  return false;
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWayland, aura::WindowTreeHost implementation:

ui::EventSource* DesktopWindowTreeHostWayland::GetEventSource() {
  return this;
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostX11, ui::EventSource implementation:

ui::EventProcessor* DesktopWindowTreeHostWayland::GetEventProcessor() {
  return dispatcher();
}

gfx::AcceleratedWidget DesktopWindowTreeHostWayland::GetAcceleratedWidget() {
  return window_;
}

void DesktopWindowTreeHostWayland::Show() {
  if (state_ & Visible)
    return;

  platform_window_->Show();
  ShowWindowWithState(ui::SHOW_STATE_NORMAL);
  native_widget_delegate_->OnNativeWidgetVisibilityChanged(true);
}

void DesktopWindowTreeHostWayland::Hide() {
  if (!(state_ & Visible))
    return;

  state_ &= ~Visible;
  platform_window_->Hide();
  native_widget_delegate_->OnNativeWidgetVisibilityChanged(false);
}

gfx::Rect DesktopWindowTreeHostWayland::GetBounds() const {
  return platform_window_->GetBounds();
}

void DesktopWindowTreeHostWayland::SetBounds(
    const gfx::Rect& requested_bounds) {
  gfx::Rect bounds(requested_bounds.origin(),
                   AdjustSize(requested_bounds.size()));
  platform_window_->SetBounds(bounds);
}

gfx::Point DesktopWindowTreeHostWayland::GetLocationOnNativeScreen() const {
  return platform_window_->GetBounds().origin();
}

void DesktopWindowTreeHostWayland::SetCapture() {
  if (HasCapture())
    return;

  DesktopWindowTreeHostWayland* old_capturer = g_current_capture;
  g_current_capture = this;
  if (old_capturer) {
    old_capturer->OnHostLostWindowCapture();
  }

  g_current_dispatcher = this;
  platform_window_->SetCapture();
}

void DesktopWindowTreeHostWayland::ReleaseCapture() {
  if (g_current_capture != this)
    return;

  platform_window_->ReleaseCapture();
  OnHostLostWindowCapture();
  g_current_capture = NULL;
  g_current_dispatcher = g_active_window;
}

void DesktopWindowTreeHostWayland::SetCursorNative(gfx::NativeCursor cursor) {
  // TODO(kalyan): Add support for custom cursor.
  ui::WindowStateChangeHandler::GetInstance()->SetWidgetCursor(
      cursor.native_type());
}

void DesktopWindowTreeHostWayland::OnCursorVisibilityChangedNative(bool show) {
  // TODO(erg): Conditional on us enabling touch on desktop linux builds, do
  // the same tap-to-click disabling here that chromeos does.
}

void DesktopWindowTreeHostWayland::MoveCursorToNative(
    const gfx::Point& location) {
  NOTIMPLEMENTED();
}

void DesktopWindowTreeHostWayland::PostNativeEvent(
    const base::NativeEvent& native_event) {
  NOTIMPLEMENTED();
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWayland, Private implementation:

void DesktopWindowTreeHostWayland::Relayout() {
  Widget* widget = native_widget_delegate_->AsWidget();
  NonClientView* non_client_view = widget->non_client_view();
  // non_client_view may be NULL, especially during creation.
  if (non_client_view) {
    non_client_view->client_view()->InvalidateLayout();
    non_client_view->InvalidateLayout();
  }
  widget->GetRootView()->Layout();
}

std::list<gfx::AcceleratedWidget>&
DesktopWindowTreeHostWayland::open_windows() {
  if (!open_windows_)
    open_windows_ = new std::list<gfx::AcceleratedWidget>();

  return *open_windows_;
}

gfx::Size DesktopWindowTreeHostWayland::AdjustSize(
    const gfx::Size& requested_size) {
  std::vector<gfx::Display> displays =
      gfx::Screen::GetScreenByType(gfx::SCREEN_TYPE_NATIVE)->GetAllDisplays();
  // Compare against all monitor sizes. The window manager can move the window
  // to whichever monitor it wants.
  for (size_t i = 0; i < displays.size(); ++i) {
    if (requested_size == displays[i].size()) {
      return gfx::Size(requested_size.width() - 1,
                       requested_size.height() - 1);
    }
  }
  return requested_size;
}

void DesktopWindowTreeHostWayland::DispatchMouseEvent(ui::MouseEvent* event) {
  // In Windows, the native events sent to chrome are separated into client
  // and non-client versions of events, which we record on our LocatedEvent
  // structures. On Desktop Ozone, we emulate the concept of non-client. Before
  // we pass this event to the cross platform event handling framework, we need
  // to make sure it is appropriately marked as non-client if it's in the non
  // client area, or otherwise, we can get into a state where the a window is
  // set as the |mouse_pressed_handler_| in window_event_dispatcher.cc
  // despite the mouse button being released.
  //
  // We can't do this later in the dispatch process because we share that
  // with ash, and ash gets confused about event IS_NON_CLIENT-ness on
  // events, since ash doesn't expect this bit to be set, because it's never
  // been set before. (This works on ash on Windows because none of the mouse
  // events on the ash desktop are clicking in what Windows considers to be a
  // non client area.) Likewise, we won't want to do the following in any
  // WindowTreeHost that hosts ash.
  if (content_window_ && content_window_->delegate()) {
    int flags = event->flags();
    int hit_test_code =
        content_window_->delegate()->GetNonClientComponent(event->location());
    if (hit_test_code != HTCLIENT && hit_test_code != HTNOWHERE)
      flags |= ui::EF_IS_NON_CLIENT;
    event->set_flags(flags);
  }

  SendEventToProcessor(event);
}

////////////////////////////////////////////////////////////////////////////////
// ui::PlatformWindowDelegate implementation:
void DesktopWindowTreeHostWayland::OnBoundChanged(
    const gfx::Rect& old_bounds, const gfx::Rect& new_bounds){
  bool origin_changed = old_bounds.origin() != new_bounds.origin();
  bool size_changed = old_bounds.size() != new_bounds.size();

  if (origin_changed)
    native_widget_delegate_->AsWidget()->OnNativeWidgetMove();

  if (size_changed)
    OnHostResized(new_bounds.size());
  else
    compositor()->ScheduleRedrawRect(new_bounds);
}

void DesktopWindowTreeHostWayland::OnActivationChanged(bool active) {
  if (active == (state_ & Active))
    return;

  if (active) {
    // Make sure the stacking order is correct. The activated window should be
    // first one in list of open windows.
    std::list<gfx::AcceleratedWidget>& windows = open_windows();
    DCHECK(windows.size());
    if (windows.front() != window_) {
      windows.remove(window_);
      windows.insert(windows.begin(), window_);
    } else {
      ReleaseCapture();
      if (g_active_window) {
        g_active_window->Deactivate();
        g_active_window = NULL;
      }
    }

    state_ |= Active;
    g_active_window = this;
    g_current_dispatcher = g_active_window;
    OnHostActivated();
  } else {
    state_ &= ~Active;
    if (g_active_window == this)
      g_active_window = NULL;

    if (g_current_dispatcher == this)
      g_current_dispatcher = NULL;
  }

  desktop_native_widget_aura_->HandleActivationChanged(active);
  native_widget_delegate_->AsWidget()->GetRootView()->SchedulePaint();
}

void DesktopWindowTreeHostWayland::OnLostCapture() {
  ReleaseCapture();
}

void DesktopWindowTreeHostWayland::OnCloseRequest() {
  Close();
}

void DesktopWindowTreeHostWayland::OnWindowStateChanged(
    ui::PlatformWindowState new_state) {
  switch (new_state) {
    case ui::PLATFORM_WINDOW_STATE_MAXIMIZED: {
      if (state_ & Minimized) {
        content_window_->Show();
        compositor()->SetVisible(true);
      }
      state_ &= ~Minimized;
      platform_window_->SetBounds(previous_bounds_);
      previous_bounds_ = gfx::Rect();
      Relayout();
      break;
    }
    default:
      break;
  }
}

void DesktopWindowTreeHostWayland::DispatchEvent(ui::Event* event) {
  SendEventToProcessor(event);
}

////////////////////////////////////////////////////////////////////////////////
// WindowTreeHostDelegateWayland, ui::PlatformEventDispatcher implementation:
bool DesktopWindowTreeHostWayland::CanDispatchEvent(
    const ui::PlatformEvent& ne) {
  DCHECK(ne);

  if (g_current_dispatcher && g_current_dispatcher == this)
    return true;

  return false;
}

uint32_t DesktopWindowTreeHostWayland::DispatchEvent(
    const ui::PlatformEvent& ne) {
  ui::EventType type = ui::EventTypeFromNative(ne);

  switch (type) {
    case ui::ET_TOUCH_MOVED:
    case ui::ET_TOUCH_PRESSED:
    case ui::ET_TOUCH_CANCELLED:
    case ui::ET_TOUCH_RELEASED: {
      ui::TouchEvent* touchev = static_cast<ui::TouchEvent*>(ne);
      SendEventToProcessor(touchev);
      break;
    }
    case ui::ET_KEY_PRESSED:
    case ui::ET_KEY_RELEASED: {
      SendEventToProcessor(static_cast<ui::KeyEvent*>(ne));
      break;
    }
    case ui::ET_MOUSEWHEEL: {
      ui::MouseWheelEvent* wheelev = static_cast<ui::MouseWheelEvent*>(ne);
      DispatchMouseEvent(wheelev);
      break;
    }
    case ui::ET_MOUSE_MOVED:
    case ui::ET_MOUSE_DRAGGED:
    case ui::ET_MOUSE_PRESSED:
    case ui::ET_MOUSE_RELEASED:
    case ui::ET_MOUSE_ENTERED:
    case ui::ET_MOUSE_EXITED: {
      ui::MouseEvent* mouseev = static_cast<ui::MouseEvent*>(ne);
      DispatchMouseEvent(mouseev);
      break;
    }
    case ui::ET_SCROLL_FLING_START:
    case ui::ET_SCROLL_FLING_CANCEL:
    case ui::ET_SCROLL: {
      SendEventToProcessor(static_cast<ui::ScrollEvent*>(ne));
      break;
    }
    case ui::ET_UMA_DATA:
      break;
    case ui::ET_UNKNOWN:
      break;
    default:
      NOTIMPLEMENTED() << "WindowTreeHostDelegateWayland: unknown event type.";
  }
  return ui::POST_DISPATCH_STOP_PROPAGATION;
}

// static
VIEWS_EXPORT ui::NativeTheme*
DesktopWindowTreeHost::GetNativeTheme(aura::Window* window) {
  const views::LinuxUI* linux_ui = views::LinuxUI::instance();
  if (linux_ui) {
    ui::NativeTheme* native_theme = linux_ui->GetNativeTheme(window);
    if (native_theme)
      return native_theme;
  }

  return ui::NativeTheme::instance();
}

}  // namespace views
