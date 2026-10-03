// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Core.h"
#include "Core/RecompWidescreen.h"
#include "Core/State.h"
#include "Core/System.h"

#include <Windows.h>
#include <climits>
#include <string>
#include <dwmapi.h>
#include <thread>  // std::this_thread::sleep_for; MSVC pulls this in indirectly

#include "VideoCommon/Present.h"
#include "VideoCommon/RecompMenu.h"
#include "resource.h"

namespace
{
class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("DolphinNoGUI");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass();
  bool CreateRenderWindow();
  void UpdateWindowPosition();
  void ProcessEvents();

  void ToggleFullscreen();

  HWND m_hwnd{};
  // Saved so leaving fullscreen restores the exact window, not a guess.
  WINDOWPLACEMENT m_saved_placement{sizeof(WINDOWPLACEMENT)};
  LONG_PTR m_saved_style = 0;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

// The window icon: the game's own, which setup.ps1 extracts from the player's
// disc into <install>\art\icon.ico (the shortcuts use the same file). It cannot
// be compiled into the exe -- the artwork is the publisher's, and this exe is
// built before any disc exists. Upstream Dolphin loads IDI_ICON1 from its .rc,
// but LoadIcon(nullptr, ...) asks for a SYSTEM icon, and ours has no icon
// resource either: players saw a yellow warning triangle in the title bar and
// taskbar. Falls back to the plain application icon.
static HICON LoadGameIcon(int size)
{
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
  {
    std::wstring path(exe, n);
    const size_t bin = path.find_last_of(L"\\/");
    if (bin != std::wstring::npos)
    {
      path.resize(bin);  // ...\bin
      const size_t root = path.find_last_of(L"\\/");
      if (root != std::wstring::npos)
      {
        path.resize(root);  // the install folder
        path += L"\\art\\icon.ico";
        if (HICON icon = static_cast<HICON>(
                LoadImageW(nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE)))
          return icon;
      }
    }
  }
  return LoadIcon(nullptr, IDI_APPLICATION);
}

bool PlatformWin32::RegisterRenderWindowClass()
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadGameIcon(GetSystemMetrics(SM_CXICON));
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Black, not COLOR_WINDOW (white): any part of the window the swapchain has
  // not covered yet -- mid-resize, or entering fullscreen -- shows this brush,
  // and a white flash round a game picture is what RingOut#10 reported.
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadGameIcon(GetSystemMetrics(SM_CXSMICON));

  // ERROR_CLASS_ALREADY_EXISTS is not a failure here. A window class is
  // registered per PROCESS and is never unregistered, so the second session in
  // one process always finds it there -- and this runtime restarts the session
  // in place to arm a recording, play a replay back, or start netplay, all of
  // which build a fresh platform. Upstream DolphinNoGUI never restarts, so the
  // bare check was correct there and fatal here.
  //
  // Reported as a crash on pressing record: what the player sees is this
  // MessageBox, "Window registration failed.", and then no game. It cost
  // nothing to reach -- the FIRST session always works, so nothing about
  // launching or playing hints that the second one cannot start.
  if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, _T("Dolphin"), WS_OVERLAPPEDWINDOW,
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

// Borderless fullscreen: take the monitor the window is currently on, drop the
// frame, and cover it. Chosen over a display-mode change because it needs no
// cooperation from the video backend and comes back cleanly if the game is
// still running -- which matters here, since the toggle arrives from the
// overlay mid-session rather than at startup.
void PlatformWin32::ToggleFullscreen()
{
  const LONG_PTR style = GetWindowLongPtr(m_hwnd, GWL_STYLE);

  // THE WHITE BORDER (RingOut#10). The window is created with WS_EX_CLIENTEDGE,
  // an EXTENDED style that draws a sunken 3D edge round the client area. Only
  // GWL_STYLE was being stripped, so that edge -- light-coloured, and backed by
  // the class's white background brush -- stayed drawn round the picture in
  // "borderless" fullscreen. Strip the edge-drawing extended styles too, and put
  // back exactly what the window had when leaving.
  constexpr LONG_PTR kEdgeExStyles =
      WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME;
  static LONG_PTR s_saved_ex_style = WS_EX_CLIENTEDGE;
  const LONG_PTR ex_style = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);

  if (!m_window_fullscreen)
  {
    MONITORINFO mi = {sizeof(MONITORINFO)};
    if (!GetWindowPlacement(m_hwnd, &m_saved_placement) ||
        !GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
    {
      return;
    }
    m_saved_style = style;
    s_saved_ex_style = ex_style;
    SetWindowLongPtr(m_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
    SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, ex_style & ~kEdgeExStyles);
    SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    m_window_fullscreen = true;
  }
  else
  {
    SetWindowLongPtr(m_hwnd, GWL_STYLE, m_saved_style ? m_saved_style : (style | WS_OVERLAPPEDWINDOW));
    SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, s_saved_ex_style);
    SetWindowPlacement(m_hwnd, &m_saved_placement);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    m_window_fullscreen = false;
    // Only meaningful outside fullscreen, and it early-returns while inside.
    UpdateWindowPosition();
  }
}

bool PlatformWin32::Init()
{
  if (!RegisterRenderWindowClass() || !CreateRenderWindow())
    return false;

  // Fullscreen and quit are window-system operations the overlay cannot perform
  // itself, so hand it callbacks into this platform -- exactly as PlatformX11
  // and PlatformWayland do. Without these the menu's Fullscreen and Quit rows
  // are silent no-ops on Windows: the overlay calls a callback nobody set.
  RecompMenu::SetFullscreenCallback([this] { ToggleFullscreen(); });
  RecompMenu::SetQuitCallback([this] { RequestShutdown(); });

  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ToggleFullscreen();
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();

    RecompMenu::HostTick();
    // While the menu is up emulation is paused, so nothing drives presentation
    // and the overlay would freeze. Redraw it here at roughly 60Hz instead --
    // the same pump PlatformX11 and PlatformWayland run.
    if (RecompMenu::IsOpen())
    {
      RecompMenu::PumpFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
  }
  break;

  // WM_SYSKEYUP as well: a key released while Alt is held arrives as the SYS
  // variant, so a plain WM_KEYUP would miss it and leave fast-forward stuck on.
  case WM_SYSKEYUP:
  case WM_KEYUP:
    if (wParam == VK_TAB)
      RecompMenu::SetFastForward(false);
    break;

  // WM_SYSKEYDOWN IS NOT OPTIONAL HERE. Windows does not deliver every key as
  // WM_KEYDOWN: anything pressed with Alt held arrives as WM_SYSKEYDOWN, and so
  // does F10 on its own, because F10 is reserved for menu-bar activation. With
  // only WM_KEYDOWN handled, Escape worked while F10 and Alt+Enter silently did
  // nothing -- which is exactly how this was found.
  case WM_SYSKEYDOWN:
  case WM_KEYDOWN:
  {
    // Mirrors PlatformX11/PlatformWayland. Windows had kept stock DolphinNoGUI
    // behaviour -- Escape quit outright -- so the overlay could never be
    // reached with a keyboard at all.
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    // Hold Tab = unlimited speed; Alt+Tab stays the window manager's. The
    // repeat flag (bit 30) is filtered so a held key does not re-trigger.
    if (wParam == VK_TAB && !alt)
    {
      if (!(lParam & (1 << 30)))
        RecompMenu::SetFastForward(true);
      break;
    }

    // Escape opens the settings overlay (and pauses) rather than quitting;
    // Shift+Escape remains an immediate quit.
    if (wParam == VK_ESCAPE)
    {
      if (shift)
        platform->RequestShutdown();
      else
        RecompMenu::OnEscape();
      break;
    }

    if (RecompMenu::IsOpen())
    {
      // Swallow navigation while the overlay owns input.
      if (wParam == VK_UP)
        RecompMenu::OnKey(RecompMenu::Key::Up);
      else if (wParam == VK_DOWN)
        RecompMenu::OnKey(RecompMenu::Key::Down);
      else if (wParam == VK_LEFT)
        RecompMenu::OnKey(RecompMenu::Key::Left);
      else if (wParam == VK_RIGHT)
        RecompMenu::OnKey(RecompMenu::Key::Right);
      else if (wParam == VK_SPACE || wParam == VK_RETURN)
        RecompMenu::OnKey(RecompMenu::Key::Activate);
      // Delete throws away the selected shot, state or replay. X11 and Wayland
      // grew this with the gallery; this platform did not exist yet when they
      // did, so it arrived here a release later and by hand. A missing key is
      // silent -- the gallery opens, the entry highlights, and nothing happens.
      else if (wParam == VK_DELETE)
        RecompMenu::OnKey(RecompMenu::Key::Delete);
      break;
    }

    if (wParam == VK_F10)
    {
      auto& system = Core::System::GetInstance();
      Core::SetState(system, Core::GetState(system) == Core::State::Running ?
                                 Core::State::Paused :
                                 Core::State::Running);
    }
    else if (wParam == VK_RETURN && alt)
    {
      platform->ToggleFullscreen();
    }
    // Alt+W toggles 16:9, as on X11 and Wayland. It arrives as WM_SYSKEYDOWN
    // (Alt is held), which is why it is also in the return-0 list below.
    else if (wParam == 'W' && alt)
    {
      if (RecompWidescreen::Toggle())
        Config::Save();
    }
    // The save-state and screenshot keys, mirroring PlatformX11. This platform
    // was written after they were, and shipping it alongside the gallery is
    // what made their absence matter: F9 is the only way to put a picture in
    // the SHOTS tab and F1-F8 the only way to fill STATES, so on Windows both
    // tabs would have opened empty forever with nothing to explain why.
    //
    // F10 is checked before these and stays pause: Windows reserves it for
    // menu activation and it arrives as WM_SYSKEYDOWN, which is why it is not
    // part of the F1-F8 run.
    else if (wParam >= VK_F1 && wParam <= VK_F8)
    {
      const int slot_number = static_cast<int>(wParam) - VK_F1 + 1;
      if (shift)
        State::Save(Core::System::GetInstance(), slot_number);
      else
        State::Load(Core::System::GetInstance(), slot_number);
    }
    else if (wParam == VK_F9)
    {
      Core::SaveScreenShot();
    }
    else if (wParam == VK_F11)
    {
      State::LoadLastSaved(Core::System::GetInstance());
    }
    else if (wParam == VK_F12)
    {
      if (shift)
        State::UndoLoadState(Core::System::GetInstance());
      else
        State::UndoSaveState(Core::System::GetInstance());
    }

    // Returning 0 for a handled WM_SYSKEYDOWN stops DefWindowProc treating it
    // as menu activation, which otherwise beeps and eats the keystroke.
    if (msg == WM_SYSKEYDOWN && (wParam == VK_F10 || (wParam == VK_RETURN && alt) ||
                                 (wParam == 'W' && alt) || wParam == VK_ESCAPE ||
                                 wParam == VK_TAB))
    {
      return 0;
    }
  }
  break;

  case WM_CLOSE:
    platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
