// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompDisplayRefresh.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#include "Common/Logging/Log.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/RecompSmoothMotionMath.h"
#include "VideoCommon/VideoBackendBase.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(HAVE_X11) && defined(HAVE_XRANDR)
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#endif

namespace RecompDisplayRefresh
{
namespace
{
std::mutex s_mutex;  // guards the window and, on X11, the private connection
WindowSystemType s_type = WindowSystemType::Headless;
void* s_window = nullptr;
std::atomic<double> s_hz{0.0};

#ifdef _WIN32
double Query()
{
  HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(s_window), MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info))
    return 0.0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    return 0.0;
  // 0 and 1 mean "the hardware default", not a rate.
  return mode.dmDisplayFrequency > 1 ? static_cast<double>(mode.dmDisplayFrequency) : 0.0;
}
#elif defined(HAVE_X11) && defined(HAVE_XRANDR)
// A connection of our own: Xlib is not thread-safe on the frontend's, and the
// pacer asks from its own thread.
Display* s_display = nullptr;

double Query()
{
  if (!s_display)
    s_display = XOpenDisplay(nullptr);
  if (!s_display)
    return 0.0;
  const Window window = static_cast<Window>(reinterpret_cast<std::uintptr_t>(s_window));
  XWindowAttributes attrs{};
  if (!XGetWindowAttributes(s_display, window, &attrs))
    return 0.0;
  int cx = 0, cy = 0;
  Window child;
  XTranslateCoordinates(s_display, window, attrs.root, attrs.width / 2, attrs.height / 2, &cx,
                        &cy, &child);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(s_display, attrs.root);
  if (!res)
    return 0.0;
  double hz = 0.0;
  for (int c = 0; c < res->ncrtc && hz == 0.0; ++c)
  {
    XRRCrtcInfo* crtc = XRRGetCrtcInfo(s_display, res, res->crtcs[c]);
    if (!crtc)
      continue;
    const bool holds = crtc->mode != None && cx >= crtc->x && cy >= crtc->y &&
                       cx < crtc->x + static_cast<int>(crtc->width) &&
                       cy < crtc->y + static_cast<int>(crtc->height);
    if (holds)
    {
      for (int m = 0; m < res->nmode; ++m)
      {
        const XRRModeInfo& mode = res->modes[m];
        if (mode.id == crtc->mode)
          hz = RecompSmoothMotion::RefreshFromMode(mode.dotClock, mode.hTotal, mode.vTotal);
      }
    }
    XRRFreeCrtcInfo(crtc);
  }
  XRRFreeScreenResources(res);
  return hz;
}
#else
double Query()
{
  return 0.0;
}
#endif
}  // namespace

void SetWindow(const WindowSystemInfo& wsi)
{
  {
    std::lock_guard lock(s_mutex);
    s_type = wsi.type;
    s_window = wsi.render_window;
  }
  INFO_LOG_FMT(VIDEO, "Smooth Motion: display refresh {:.2f} Hz", Refresh());
}

double Refresh()
{
  std::lock_guard lock(s_mutex);
  double hz = 0.0;
#ifdef _WIN32
  if (s_type == WindowSystemType::Windows && s_window)
    hz = Query();
#else
  if (s_type == WindowSystemType::X11 && s_window)
    hz = Query();
#endif
  s_hz.store(hz, std::memory_order_relaxed);
  return hz;
}

double Current()
{
  const int forced = Config::Get(Config::GFX_SMOOTH_MOTION_REFRESH_HZ);
  if (forced > 0)
    return static_cast<double>(forced);
  return s_hz.load(std::memory_order_relaxed);
}

bool SmoothMotionOffered()
{
  const std::string backend =
      g_video_backend ? g_video_backend->GetConfigName() : Config::Get(Config::MAIN_GFX_BACKEND);
  return RecompSmoothMotion::Offered(backend == "Vulkan", Current());
}
}  // namespace RecompDisplayRefresh
