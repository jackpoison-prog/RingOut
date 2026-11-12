// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/RecompWidescreen.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/EFBInterface.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RecompMenu.h"

#include <AppKit/AppKit.h>
#include <Carbon/Carbon.h>
#include <Foundation/Foundation.h>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <thread>

@interface Application : NSApplication
@property Platform* platform;
- (void)shutdown;
- (void)togglePause;
- (void)saveScreenShot;
- (void)loadLastSaved;
- (void)undoLoadState;
- (void)undoSaveState;
- (void)loadState:(id)sender;
- (void)saveState:(id)sender;
@end

@implementation Application
- (void)shutdown;
{
  [self platform]->Stop();
  [self stop:nil];
}

- (void)togglePause
{
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Running)
    Core::SetState(system, Core::State::Paused);
  else
    Core::SetState(system, Core::State::Running);
}

- (void)saveScreenShot
{
  Core::SaveScreenShot();
}

- (void)loadLastSaved
{
  State::LoadLastSaved(Core::System::GetInstance());
}

- (void)undoLoadState
{
  State::UndoLoadState(Core::System::GetInstance());
}

- (void)undoSaveState
{
  State::UndoSaveState(Core::System::GetInstance());
}

- (void)loadState:(id)sender
{
  State::Load(Core::System::GetInstance(), [sender tag]);
}

- (void)saveState:(id)sender
{
  State::Save(Core::System::GetInstance(), [sender tag]);
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>

@property(readonly) Platform* platform;

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender;
- (id)initWithPlatform:(Platform*)platform;
@end

@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
  return YES;
}

- (id)initWithPlatform:(Platform*)platform
{
  self = [super init];
  if (self)
  {
    _platform = platform;
  }
  return self;
}
@end

@interface WindowDelegate : NSObject <NSWindowDelegate>
{
  BOOL m_cursor_hidden;
}

- (void)windowDidResize:(NSNotification*)notification;
- (void)mouseEntered:(NSEvent*)event;
- (void)mouseExited:(NSEvent*)event;
- (void)windowWillClose:(NSNotification*)notification;
@end

@implementation WindowDelegate

- (id)init
{
  self = [super init];
  if (self)
  {
    m_cursor_hidden = NO;
  }
  return self;
}

- (void)windowDidResize:(NSNotification*)notification
{
  if (g_presenter)
    g_presenter->ResizeSurface();
}

- (void)mouseEntered:(NSEvent*)event
{
  if (!m_cursor_hidden)
  {
    [NSCursor hide];
    m_cursor_hidden = YES;
  }
}

- (void)mouseExited:(NSEvent*)event
{
  if (m_cursor_hidden)
  {
    [NSCursor unhide];
    m_cursor_hidden = NO;
  }
}

- (void)windowWillClose:(NSNotification*)notification
{
  if (m_cursor_hidden)
  {
    [NSCursor unhide];
    m_cursor_hidden = NO;
  }
}
@end

namespace
{
class PlatformMacOS : public Platform
{
public:
  ~PlatformMacOS() override;

  bool Init() override;
  void SetTitle(const std::string& title) override;
  void MainLoop() override;
  void SaveWindowGeometry() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  void ProcessEvents();
  // RingOut's keys (the same set as PlatformX11/Wayland/Win32). True when the event was ours and
  // must not reach AppKit (which would beep, or leave native fullscreen on Escape).
  bool HandleKey(NSEvent* event);
  void ToggleFullscreen();
  void UpdateWindowPosition();
  void HandleSaveStates(NSUInteger key, NSUInteger flags);
  void SetupMenu();

  NSRect m_window_rect;
  NSWindow* m_window;
  NSMenu* menuBar;
  AppDelegate* m_app_delegate;
  WindowDelegate* m_window_delegate;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  unsigned int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  unsigned int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
  bool m_window_fullscreen = Config::Get(Config::MAIN_FULLSCREEN);
};

PlatformMacOS::~PlatformMacOS()
{
  [m_window close];
}

bool PlatformMacOS::Init()
{
  // Fullscreen and quit are window-system operations the overlay can't perform itself, so hand it
  // callbacks into this platform. The overlay calls them from OnKey, on this (the main) thread.
  RecompMenu::SetFullscreenCallback([this] { ToggleFullscreen(); });
  RecompMenu::SetQuitCallback([this] { RequestShutdown(); });

  [Application sharedApplication];

  m_app_delegate = [[AppDelegate alloc] initWithPlatform:this];
  [NSApp setDelegate:m_app_delegate];

  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp setPlatform:this];
  [Application.sharedApplication finishLaunching];

  unsigned long styleMask =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;

  m_window_rect = CGRectMake(m_window_x, m_window_y, m_window_width, m_window_height);
  m_window = [NSWindow alloc];
  m_window = [m_window initWithContentRect:m_window_rect
                                 styleMask:styleMask
                                   backing:NSBackingStoreBuffered
                                     defer:NO];
  if (m_window_x == -1 || m_window_y == -1)
  {
    [m_window center];
  }
  m_window_delegate = [[WindowDelegate alloc] init];
  [m_window setDelegate:m_window_delegate];

  NSTrackingAreaOptions trackingOptions =
      NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect;
  NSTrackingArea* trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                             options:trackingOptions
                                                               owner:m_window_delegate
                                                            userInfo:nil];
  [[m_window contentView] addTrackingArea:trackingArea];

  NSNotificationCenter* c = [NSNotificationCenter defaultCenter];
  [c addObserver:NSApp
        selector:@selector(shutdown)
            name:NSWindowWillCloseNotification
          object:m_window];

  if (m_window == nil)
  {
    NSLog(@"Window is %@\n", m_window);
    return false;
  }

  if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
    [NSCursor hide];

  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    m_window_fullscreen = true;
    [m_window toggleFullScreen:m_window];
  }

  [m_window makeKeyAndOrderFront:NSApp];
  [m_window makeMainWindow];
  [NSApp activateIgnoringOtherApps:YES];
  [m_window setTitle:@"Dolphin-emu-nogui"];

  SetupMenu();

  return true;
}

void PlatformMacOS::SetTitle(const std::string& title)
{
  @autoreleasepool
  {
    NSWindow* window = m_window;
    NSString* str = [NSString stringWithUTF8String:title.c_str()];
    dispatch_async(dispatch_get_main_queue(), ^{
      [window setTitle:str];
    });
  }
}

void PlatformMacOS::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();

    RecompMenu::HostTick();
    // While the menu is up emulation is paused, so nothing drives presentation and the overlay
    // would freeze. Redraw it here at roughly 60Hz instead.
    if (RecompMenu::IsOpen())
    {
      RecompMenu::PumpFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void PlatformMacOS::ToggleFullscreen()
{
  [m_window toggleFullScreen:nil];
}

bool PlatformMacOS::HandleKey(NSEvent* event)
{
  const NSEventType type = [event type];
  if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp)
    return false;
  const unsigned short key = [event keyCode];
  const NSEventModifierFlags mods = [event modifierFlags];
  const bool option = (mods & NSEventModifierFlagOption) != 0;
  const bool shift = (mods & NSEventModifierFlagShift) != 0;
  const bool command = (mods & NSEventModifierFlagCommand) != 0;

  if (type == NSEventTypeKeyUp)
  {
    if (key == kVK_Tab)
    {
      RecompMenu::SetFastForward(false);
      return true;
    }
    return false;
  }

  // Hold Tab = unlimited speed (key repeats only re-assert it); Cmd+Tab stays the system's.
  if (key == kVK_Tab && !command && !option)
  {
    RecompMenu::SetFastForward(true);
    return true;
  }
  // Escape opens the settings overlay (and pauses); Shift+Escape quits.
  if (key == kVK_Escape)
  {
    if ([event isARepeat])
      return true;
    if (shift)
      RequestShutdown();
    else
      RecompMenu::OnEscape();
    return true;
  }
  if (RecompMenu::IsOpen())
  {
    // The overlay owns input: navigate it and swallow everything else.
    switch (key)
    {
    case kVK_UpArrow:
      RecompMenu::OnKey(RecompMenu::Key::Up);
      break;
    case kVK_DownArrow:
      RecompMenu::OnKey(RecompMenu::Key::Down);
      break;
    case kVK_LeftArrow:
      RecompMenu::OnKey(RecompMenu::Key::Left);
      break;
    case kVK_RightArrow:
      RecompMenu::OnKey(RecompMenu::Key::Right);
      break;
    case kVK_Space:
    case kVK_Return:
    case kVK_ANSI_KeypadEnter:
      if (![event isARepeat])
        RecompMenu::OnKey(RecompMenu::Key::Activate);
      break;
    case kVK_ForwardDelete:
    case kVK_Delete:  // the Mac's "delete" key is backspace; a laptop has no forward delete
      if (![event isARepeat])
        RecompMenu::OnKey(RecompMenu::Key::Delete);
      break;
    default:
      break;
    }
    return true;
  }
  // Option is the Mac's Alt: Option+Return = fullscreen, Option+W = 16:9, as Alt does elsewhere.
  if (option && !command && (key == kVK_Return || key == kVK_ANSI_KeypadEnter))
  {
    if (![event isARepeat])
      ToggleFullscreen();
    return true;
  }
  if (option && !command && key == kVK_ANSI_W)
  {
    if (![event isARepeat] && RecompWidescreen::Toggle())
      Config::Save();
    return true;
  }
  return false;
}

WindowSystemInfo PlatformMacOS::GetWindowSystemInfo() const
{
  @autoreleasepool
  {
    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::MacOS;
    wsi.render_window = (void*)CFBridgingRetain([m_window contentView]);
    wsi.render_surface = wsi.render_window;
    return wsi;
  }
}

void PlatformMacOS::ProcessEvents()
{
  @autoreleasepool
  {
    // Drain what is pending without waiting: MainLoop paces itself (and must keep pumping the
    // overlay while the game is paused), where the stock loop blocked here for up to a second.
    while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:[NSDate distantPast]
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES])
    {
      if (HandleKey(event))
        continue;
      // Other keys: menu shortcuts (Cmd+Q, the F-key states) still work, and the rest stop here.
      // The game reads the keyboard by polling (QuartzKeyboardAndMouse, CGEventSourceKeyState),
      // and a key event no view accepts makes AppKit beep on every press.
      if ([event type] == NSEventTypeKeyDown)
      {
        [[NSApp mainMenu] performKeyEquivalent:event];
        continue;
      }
      if ([event type] == NSEventTypeKeyUp)
        continue;
      [NSApp sendEvent:event];
    }

    // Need to update if m_window becomes fullscreen
    m_window_fullscreen = [m_window styleMask] & NSWindowStyleMaskFullScreen;

    if ([m_window isMainWindow])
    {
      m_window_focus = true;
      if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never &&
          Core::GetState(Core::System::GetInstance()) != Core::State::Paused)
      {
        [NSCursor unhide];
      }
    }
    else
    {
      // Losing focus mid-hold means the Tab release will never arrive.
      if (m_window_focus)
        RecompMenu::SetFastForward(false);
      m_window_focus = false;
      if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
        [NSCursor hide];
    }
  }
}

void PlatformMacOS::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  NSRect win = [m_window contentRectForFrameRect:[m_window frame]];
  m_window_x = win.origin.x;
  m_window_y = win.origin.y;
  m_window_width = win.size.width;
  m_window_height = win.size.height;
}

void PlatformMacOS::SetupMenu()
{
  @autoreleasepool
  {
    menuBar = [NSMenu new];

    NSMenu* appMenu = [NSMenu new];
    NSMenu* stateMenu = [[NSMenu alloc] initWithTitle:@"States"];
    NSMenu* loadStateMenu = [[NSMenu alloc] initWithTitle:@"Load"];
    NSMenu* saveStateMenu = [[NSMenu alloc] initWithTitle:@"Save"];
    NSMenu* miscMenu = [[NSMenu alloc] initWithTitle:@"Misc"];

    NSMenuItem* appMenuItem = [NSMenuItem new];
    NSMenuItem* miscMenuItem = [NSMenuItem new];
    NSMenuItem* stateMenuItem = [NSMenuItem new];
    NSMenuItem* loadStateItem = [[NSMenuItem alloc] initWithTitle:@"Load"
                                                           action:nil
                                                    keyEquivalent:@""];
    NSMenuItem* saveStateItem = [[NSMenuItem alloc] initWithTitle:@"Save"
                                                           action:nil
                                                    keyEquivalent:@""];
    [menuBar addItem:appMenuItem];
    [menuBar addItem:stateMenuItem];
    [menuBar addItem:miscMenuItem];

    // Quit
    NSString* quitTitle = [@"Quit " stringByAppendingString:@"dolphin-emu-nogui"];
    NSMenuItem* quitMenuItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                          action:@selector(shutdown)
                                                   keyEquivalent:@"q"];

    // Fullscreen
    NSString* fullScreenItemTitle = @"Toggle Fullscreen";
    NSMenuItem* fullScreenItem = [[NSMenuItem alloc] initWithTitle:fullScreenItemTitle
                                                            action:@selector(toggleFullScreen:)
                                                     keyEquivalent:@"f"];
    [fullScreenItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Screenshot
    NSString* ScreenShotTitle = @"Take Screenshot";
    unichar c = NSF9FunctionKey;
    NSString* f9 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* ScreenShotItem = [[NSMenuItem alloc] initWithTitle:ScreenShotTitle
                                                            action:@selector(saveScreenShot)
                                                     keyEquivalent:f9];
    [ScreenShotItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Pause game
    NSString* pauseTitle = @"Toggle pause";
    c = NSF10FunctionKey;
    NSString* f10 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* pauseItem = [[NSMenuItem alloc] initWithTitle:pauseTitle
                                                       action:@selector(togglePause)
                                                keyEquivalent:f10];
    [pauseItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Load last save
    NSString* loadLastTitle = @"Load Last Saved";
    c = NSF11FunctionKey;
    NSString* f11 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* loadLastItem = [[NSMenuItem alloc] initWithTitle:loadLastTitle
                                                          action:@selector(loadLastSaved)
                                                   keyEquivalent:f11];
    [loadLastItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Undo Load State
    NSString* undoLoadTitle = @"Undo Load";
    c = NSF12FunctionKey;
    NSString* f12 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* undoLoadItem = [[NSMenuItem alloc] initWithTitle:undoLoadTitle
                                                          action:@selector(undoLoadState)
                                                   keyEquivalent:f12];
    [undoLoadItem setKeyEquivalentModifierMask:NSEventModifierFlagShift];

    // Undo Save State
    NSString* undoSaveTitle = @"Undo Save";
    NSMenuItem* undoSaveItem = [[NSMenuItem alloc] initWithTitle:undoSaveTitle
                                                          action:@selector(undoSaveState)
                                                   keyEquivalent:f12];
    [undoSaveItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Load and Save States
    for (unichar i = NSF1FunctionKey; i <= NSF8FunctionKey; i++)
    {
      NSInteger stateNum = i - NSF1FunctionKey + 1;
      NSString* lstateTitle = [NSString stringWithFormat:@"Load State %ld", (long)stateNum];
      c = i;
      NSString* t = [NSString stringWithCharacters:&c length:1];
      NSMenuItem* lstateItem = [[NSMenuItem alloc] initWithTitle:lstateTitle
                                                          action:@selector(loadState:)
                                                   keyEquivalent:t];
      [lstateItem setTag:stateNum];
      [lstateItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];
      [loadStateMenu addItem:lstateItem];

      NSString* sstateTitle = [NSString stringWithFormat:@"Save State %ld", (long)stateNum];
      c = i;
      NSMenuItem* sstateItem = [[NSMenuItem alloc] initWithTitle:sstateTitle
                                                          action:@selector(saveState:)
                                                   keyEquivalent:t];
      [sstateItem setKeyEquivalentModifierMask:NSEventModifierFlagShift];
      [sstateItem setTag:stateNum];
      [saveStateMenu addItem:sstateItem];
    }

    // App Main menu
    [appMenu addItem:quitMenuItem];

    // State Menu
    [loadStateItem setSubmenu:loadStateMenu];
    [saveStateItem setSubmenu:saveStateMenu];

    [stateMenu addItem:loadLastItem];
    [stateMenu addItem:undoLoadItem];
    [stateMenu addItem:undoSaveItem];
    [stateMenu addItem:loadStateItem];
    [stateMenu addItem:saveStateItem];

    // Misc Menu
    [miscMenu addItem:fullScreenItem];
    [miscMenu addItem:ScreenShotItem];
    [miscMenu addItem:pauseItem];

    [appMenuItem setSubmenu:appMenu];
    [stateMenuItem setSubmenu:stateMenu];
    [miscMenuItem setSubmenu:miscMenu];

    [NSApp setMainMenu:menuBar];
  }
}

void PlatformMacOS::SaveWindowGeometry()
{
  if (Config::Get(Config::MAIN_RENDER_WINDOW_SAVE_ON_EXIT))
  {
    Config::SetBaseOrCurrent(Config::MAIN_RENDER_WINDOW_WIDTH, static_cast<int>(m_window_width));
    Config::SetBaseOrCurrent(Config::MAIN_RENDER_WINDOW_HEIGHT, static_cast<int>(m_window_height));
    Config::SetBaseOrCurrent(Config::MAIN_RENDER_WINDOW_XPOS, static_cast<int>(m_window_x));
    Config::SetBaseOrCurrent(Config::MAIN_RENDER_WINDOW_YPOS, static_cast<int>(m_window_y));
    Config::Save();
  }
}

}  // namespace

std::unique_ptr<Platform> Platform::CreateMacOSPlatform()
{
  return std::make_unique<PlatformMacOS>();
}
