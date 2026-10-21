// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Core.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>
#include <variant>

#include <fmt/chrono.h>
#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "AudioCommon/AudioCommon.h"

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/OneShotEvent.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/TimeUtil.h"
#include "Common/Version.h"

#include "Core/Achievements/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiiSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FreeLookManager.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/HW.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/IOS/IOS.h"
#include "Core/MemTools.h"
#include "Core/Movie.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayProto.h"
#include "Core/RecompDeterminism.h"
#include "Core/RecompWidescreen.h"
#include "Core/Cheats/PatchEngine.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/Wii/WiiRoot.h"

#ifdef USE_MEMORYWATCHER
#include "Core/Cheats/MemoryWatcher.h"
#endif

#include "DiscIO/RiivolutionPatcher.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

namespace Core
{
static bool s_wants_determinism;

// Declarations and definitions
static std::thread s_emu_thread;
static Common::HookableEvent<Core::State> s_state_changed_event;

static bool s_is_throttler_temp_disabled = false;
static bool s_frame_step = false;
static std::atomic<bool> s_stop_frame_step;

// Threads other than the CPU thread must hold this when taking on the role of the CPU thread.
// The CPU thread is not required to hold this when doing normal work, but must hold it if writing
// to s_state.
std::recursive_mutex s_core_mutex;

// The value Paused is never stored in this variable. The core is considered to be in
// the Paused state if this variable is Running and the CPU reports that it's stepping.
std::atomic<State> s_state = State::Uninitialized;

#ifdef USE_MEMORYWATCHER
static std::unique_ptr<MemoryWatcher> s_memory_watcher;
#endif

static void Callback_FramePresented(const PresentInfo& present_info);

static void EmuThread(Core::System& system, std::unique_ptr<BootParameters> boot,
                      WindowSystemInfo wsi);

bool GetIsThrottlerTempDisabled()
{
  return s_is_throttler_temp_disabled;
}

void SetIsThrottlerTempDisabled(bool disable)
{
  s_is_throttler_temp_disabled = disable;
}

void FrameUpdateOnCPUThread()
{
  if (NetPlay::IsNetPlayRunning())
    NetPlay::NetPlayClient::SendTimeBase();

  // On the CPU thread and once per emulated frame, which is the only place a
  // state hash is comparable between two runs.
  RecompDeterminism::OnFrame(Core::System::GetInstance());

  // Same thread and cadence, for the same reason: it reads and writes guest RAM.
  RecompWidescreen::OnFrame(Core::System::GetInstance());
}

void OnFrameEnd(Core::System& system)
{
#ifdef USE_MEMORYWATCHER
  if (s_memory_watcher)
  {
    ASSERT(IsCPUThread());
    const CPUThreadGuard guard(system);

    s_memory_watcher->Step(guard);
  }
#endif
}

// Display messages and return values

// Formatted stop message
std::string StopMessage(bool main_thread, std::string_view message)
{
  return fmt::format("Stop [{} {}]\t{}", main_thread ? "Main Thread" : "Video Thread",
                     Common::CurrentThreadId(), message);
}

void DisplayMessage(std::string message, int time_in_ms)
{
  if (!IsRunningOrStarting(Core::System::GetInstance()))
    return;

  // Actually displaying non-ASCII could cause things to go pear-shaped
  if (!std::ranges::all_of(message, Common::IsPrintableCharacter))
    return;

  OSD::AddMessage(std::move(message), time_in_ms);
}

bool IsRunning(Core::System& system)
{
  return s_state.load() == State::Running;
}

bool IsRunningOrStarting(Core::System& system)
{
  const State state = s_state.load();
  return state == State::Running || state == State::Starting;
}

bool IsUninitialized(Core::System& system)
{
  return s_state.load() == State::Uninitialized;
}



bool WantsDeterminism()
{
  return s_wants_determinism;
}

// This is called from the GUI thread. See the booting call schedule in
// BootManager.cpp
bool Init(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi)
{
  std::lock_guard lock(s_core_mutex);

  if (s_emu_thread.joinable())
  {
    if (!IsUninitialized(system))
    {
      PanicAlertFmtT("Emu Thread already running");
      return false;
    }

    // The Emu Thread was stopped, synchronize with it.
    s_emu_thread.join();
  }

  // Drain any left over jobs
  HostDispatchJobs(system);

  INFO_LOG_FMT(BOOT, "Starting core = {} mode", system.IsWii() ? "Wii" : "GameCube");
  INFO_LOG_FMT(BOOT, "CPU Thread separate = {}", system.IsDualCoreMode() ? "Yes" : "No");

  // Manually reactivate the video backend in case a GameINI overrides the video backend setting.
  VideoBackendBase::PopulateBackendInfo(wsi);

  // Issue any API calls which must occur on the main thread for the graphics backend.
  WindowSystemInfo prepared_wsi(wsi);
  g_video_backend->PrepareWindow(prepared_wsi);

  // Start the emu thread
  s_state.store(State::Starting);
  s_emu_thread = std::thread(EmuThread, std::ref(system), std::move(boot), prepared_wsi);
  return true;
}

static void ResetRumble()
{
#if defined(__LIBUSB__)
  GCAdapter::ResetRumble();
#endif
  if (!Pad::IsInitialized())
    return;
  for (int i = 0; i < 4; ++i)
    Pad::ResetRumble(i);
}

// Called from GUI thread
void Stop(Core::System& system)  // - Hammertime!
{
  {
    std::lock_guard lock(s_core_mutex);

    const State state = s_state.load();
    if (state == State::Stopping || state == State::Uninitialized)
      return;

    s_state.store(State::Stopping);
  }

  NotifyStateChanged(State::Stopping);

  AchievementManager::GetInstance().CloseGame();

  // Dump left over jobs
  HostDispatchJobs(system);

  system.GetFifo().EmulatorState(false);

  INFO_LOG_FMT(CONSOLE, "Stop [Main Thread]\t\t---- Shutting down ----");

  // Stop the CPU
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Stop CPU"));
  system.GetCPU().Stop();
}



// For the CPU Thread only.
static void CPUSetInitialExecutionState(Core::System& system, bool force_paused = false)
{
  // The CPU starts in stepping state, and will wait until a new state is set before executing.
  const bool paused = SConfig::GetInstance().bBootToPause || force_paused;
  SetState(system, paused ? State::Paused : State::Running, true, true);
  Host_UpdateDisasmDialog();
}

// The emulation's two hot threads go above the rest of the process (Windows only). On a 2-core laptop
// (A4-9125) the CPU and video threads share both cores with the Vulkan submission, audio, input and OS
// threads; raising these two to ABOVE_NORMAL measured +4.9% fight fps there (three runs each, no
// overlap). Raising a thread on Linux needs CAP_SYS_NICE, so it is left alone there.
//
// The CPU thread goes one step higher than the video thread. It is the critical path, and at equal
// priority a kernel trace of a fight on that laptop showed it ready to run but switched out 10% of
// the time. CPU thread HIGHEST over the video thread's ABOVE_NORMAL measured +4.9% fight fps there
// (six runs each); lowering the video thread to NORMAL instead measured +3.0 to +3.3%.
// Single-core keeps ABOVE_NORMAL: its one thread does both jobs.
static void RaiseEmulationThreadPriority(bool highest)
{
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(),
                    highest ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

// Create the CPU thread, which is a CPU + Video thread in Single Core mode.
static void CpuThread(Core::System& system, const std::optional<std::string>& savestate_path,
                      bool delete_savestate)
{
  if (system.IsDualCoreMode())
    Common::SetCurrentThreadName("CPU thread");
  else
    Common::SetCurrentThreadName("CPU-GPU thread");
  RaiseEmulationThreadPriority(system.IsDualCoreMode());

  // This needs to be delayed until after the video backend is ready.
  DolphinAnalytics::Instance().ReportGameStart();

  // Clear performance data collected from previous threads.
  system.GetPerfMetrics().Reset();

  // The JIT need to be able to intercept faults, both for fastmem and for the BLR optimization.
  const bool exception_handler = EMM::IsExceptionHandlerSupported();
  if (exception_handler)
    EMM::InstallExceptionHandler();

#ifdef USE_MEMORYWATCHER
  s_memory_watcher = std::make_unique<MemoryWatcher>();
#endif

  if (savestate_path)
  {
    ::State::LoadAs(system, *savestate_path);
    if (delete_savestate)
      File::Delete(*savestate_path);
  }

  {
    std::unique_lock core_lock(s_core_mutex);

    // If s_state is Starting, change it to Running. But if it's already been set to Stopping
    // because another thread called Stop, don't change it.
    State expected = State::Starting;
    s_state.compare_exchange_strong(expected, State::Running);
  }

  {
#ifndef _WIN32
    std::string gdb_socket = Config::Get(Config::MAIN_GDB_SOCKET);
    if (!gdb_socket.empty() && !AchievementManager::GetInstance().IsHardcoreModeActive())
    {
      GDBStub::InitLocal(gdb_socket.data());
      CPUSetInitialExecutionState(system, true);
    }
    else
#endif
    {
      int gdb_port = Config::Get(Config::MAIN_GDB_PORT);
      if (gdb_port > 0 && !AchievementManager::GetInstance().IsHardcoreModeActive())
      {
        GDBStub::Init(gdb_port);
        CPUSetInitialExecutionState(system, true);
      }
      else
      {
        CPUSetInitialExecutionState(system);
      }
    }
  }

  // Enter CPU run loop. When we leave it - we are done.
  system.GetCPU().Run();

#ifdef USE_MEMORYWATCHER
  s_memory_watcher.reset();
#endif

  if (exception_handler)
    EMM::UninstallExceptionHandler();

  if (GDBStub::IsActive())
  {
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Stopping GDB ..."));
    GDBStub::Deinit();
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "GDB stopped."));
    INFO_LOG_FMT(GDB_STUB, "Killed by CPU shutdown");
  }
}

static void FifoPlayerThread(Core::System& system, const std::optional<std::string>& savestate_path,
                             bool delete_savestate)
{
  if (system.IsDualCoreMode())
    Common::SetCurrentThreadName("FIFO player thread");
  else
    Common::SetCurrentThreadName("FIFO-GPU thread");

  // Enter CPU run loop. When we leave it - we are done.
  if (auto cpu_core = system.GetFifoPlayer().GetCPUCore())
  {
    system.GetPowerPC().InjectExternalCPUCore(cpu_core.get());

    {
      std::lock_guard core_lock(s_core_mutex);

      // If s_state is Starting, change it to Running. But if it's already been set to Stopping
      // because another thread called Stop, don't change it.
      State expected = State::Starting;
      s_state.compare_exchange_strong(expected, State::Running);
    }

    CPUSetInitialExecutionState(system);

    system.GetCPU().Run();

    system.GetPowerPC().InjectExternalCPUCore(nullptr);
    system.GetFifoPlayer().Close();
  }
  else
  {
    // FIFO log does not contain any frames, cannot continue.
    PanicAlertFmt("FIFO file is invalid, cannot playback.");
    system.GetFifoPlayer().Close();
    return;
  }
}

// Returns a RAII object for video backend initialization and deinitialization.
// Returns nullptr on failure.
[[nodiscard]] static auto GetInitializedVideoGuard(Core::System& system,
                                                   const WindowSystemInfo& wsi)
{
  using GuardType = Common::ScopeGuard<Common::MoveOnlyFunction<void()>>;
  using ReturnType = std::unique_ptr<GuardType>;

  const auto init_video = [&] {
    DeclareAsGPUThread();

    AsyncRequests::GetInstance()->SetPassthrough(!system.IsDualCoreMode());

    // Must happen on the proper thread for some video backends, e.g. OpenGL.
    return g_video_backend->Initialize(wsi);
  };

  const auto deinit_video = [] {
    // Clear on screen messages that haven't expired
    OSD::ClearMessages();

    g_video_backend->Shutdown();
  };

  if (system.IsDualCoreMode())
  {
    std::promise<bool> init_from_thread;

    // Spawn the GPU thread.
    std::thread gpu_thread{[&] {
      Common::SetCurrentThreadName("Video thread");
      RaiseEmulationThreadPriority(false);

      const bool is_init = init_video();
      init_from_thread.set_value(is_init);

      if (!is_init)
        return;

      system.GetFifo().RunGpuLoop();
      INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "Video Loop Ended"));

      deinit_video();
    }};

    if (init_from_thread.get_future().get())
    {
      // Return a scope guard that signals the GPU thread to stop then joins it.
      return std::make_unique<GuardType>([&, gpu_thread = std::move(gpu_thread)]() mutable {
        INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Wait for Video Loop to exit ..."));
        system.GetFifo().ExitGpuLoop();
        gpu_thread.join();
        INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "GPU thread stopped."));
      });
    }

    gpu_thread.join();
  }
  else  // SingleCore mode
  {
    if (init_video())
      return std::make_unique<GuardType>(deinit_video);
  }

  return ReturnType{};
}

// Initialize and create emulation thread
// Call browser: Init():s_emu_thread().
// See the BootManager.cpp file description for a complete call schedule.
static void EmuThread(Core::System& system, std::unique_ptr<BootParameters> boot,
                      WindowSystemInfo wsi)
{
  NotifyStateChanged(State::Starting);
  Common::ScopeGuard flag_guard{[] {
    {
      std::lock_guard lock(s_core_mutex);
      s_state.store(State::Uninitialized);
    }

    NotifyStateChanged(State::Uninitialized);

    INFO_LOG_FMT(CONSOLE, "Stop\t\t---- Shutdown complete ----");
  }};

  Common::SetCurrentThreadName("Emuthread - Starting");

  // This will become the CPU thread.
  DeclareAsCPUThread();

  s_frame_step = false;

  // If settings have changed since the previous run, notify callbacks.
  CPUThreadConfigCallback::CheckForConfigChanges();

  // Switch the window used for inputs to the render window. This way, the cursor position
  // is relative to the render window, instead of the main window.
  ASSERT(g_controller_interface.IsInit());
  g_controller_interface.ChangeWindow(wsi.render_window);

  Pad::LoadConfig();
  Pad::LoadGBAConfig();
  Keyboard::LoadConfig();

  BootSessionData boot_session_data = std::move(boot->boot_session_data);
  const std::optional<std::string>& savestate_path = boot_session_data.GetSavestatePath();
  const bool delete_savestate =
      boot_session_data.GetDeleteSavestate() == DeleteSavestateAfterBoot::Yes;

  bool sync_sd_folder = system.IsWii() && Config::Get(Config::MAIN_WII_SD_CARD) &&
                        Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
  if (sync_sd_folder)
  {
    sync_sd_folder = Common::SyncSDFolderToSDImage([] { return false; }, Core::WantsDeterminism());
  }

  Common::ScopeGuard sd_folder_sync_guard{[sync_sd_folder] {
    if (sync_sd_folder && Config::Get(Config::MAIN_ALLOW_SD_WRITES))
    {
      const bool sync_ok = Common::SyncSDImageToSDFolder([] { return false; });
      if (!sync_ok)
      {
        PanicAlertFmtT(
            "Failed to sync SD card with folder. All changes made this session will be "
            "discarded on next boot if you do not manually re-issue a resync in Config > "
            "Wii > SD Card Settings > {0}!",
            Common::GetStringT(Common::SD_UNPACK_TEXT));
      }
    }
  }};

  // Wiimote input config is loaded in OnESTitleChanged

  FreeLook::LoadInputConfig();

  system.GetMovie().Init(*boot);
  Common::ScopeGuard movie_guard([&system] { system.GetMovie().Shutdown(); });

  AudioCommon::InitSoundStream(system);
  Common::ScopeGuard audio_guard([&system] { AudioCommon::ShutdownSoundStream(system); });

  HW::Init(system,
           NetPlay::IsNetPlayRunning() ? &(boot_session_data.GetNetplaySettings()->sram) : nullptr);

  Common::ScopeGuard hw_guard{[&system] {
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "Shutting down HW"));
    HW::Shutdown(system);
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "HW shutdown"));

    // The config must be restored only after the whole HW has shut down,
    // not when it is still running.
    BootManager::RestoreConfig();

    PatchEngine::Shutdown();
    HLE::Clear();

    CPUThreadGuard guard(system);
    system.GetPowerPC().GetDebugInterface().Clear(guard);
  }};

  // In single-core mode: This holds a video backend shutdown function.
  // In dual-core mode: This holds a GPU thread stopping function (which does the backend shutdown).
  const auto video_guard = GetInitializedVideoGuard(system, wsi);
  if (!video_guard)
  {
    PanicAlertFmt("Failed to initialize video backend!");
    return;
  }

  // Upstream derived MAIN_DSP_THREAD from the host CPU here:
  //
  //   if (cpu_info.HTT) SetBaseOrCurrent(MAIN_DSP_THREAD, num_cores > 4);
  //   else              SetBaseOrCurrent(MAIN_DSP_THREAD, num_cores > 2);
  //
  // That makes an EMULATION setting depend on the machine it runs on, and it
  // overwrote whatever the configuration asked for. Netplay does not sync this
  // one -- NetPlayServer sends dsp_hle and dsp_enable_jit and nothing else --
  // so a 16-core desktop and a 4-core laptop would boot the same session with
  // different DSP emulation and no warning anywhere.
  //
  // The configured value now stands. It is set explicitly in
  // DolphinRuntime::ApplyConfiguration, so it is identical on every machine.

  if (!system.GetDSP().GetDSPEmulator()->Initialize(system.IsWii(),
                                                    Config::Get(Config::MAIN_DSP_THREAD)))
  {
    PanicAlertFmt("Failed to initialize DSP emulation!");
    return;
  }

  AudioCommon::PostInitSoundStream(system);

  // Set execution state to known values (CPU/FIFO/Audio Paused)
  system.GetCPU().Break();

  // Load GCM/DOL/ELF whatever ... we boot with the interpreter core
  system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);

  // Determine the CPU thread function
  const auto cpu_thread_func =
      std::holds_alternative<BootParameters::DFF>(boot->parameters) ? FifoPlayerThread : CpuThread;

  std::optional<DiscIO::Riivolution::SavegameRedirect> savegame_redirect = std::nullopt;
  if (system.IsWii())
    savegame_redirect = DiscIO::Riivolution::ExtractSavegameRedirect(boot->riivolution_patches);

  {
    ASSERT(IsCPUThread());
    CPUThreadGuard guard(system);
    if (!CBoot::BootUp(system, guard, std::move(boot)))
      return;
  }

  // Initialise Wii filesystem contents.
  // This is done here after Boot and not in BootManager to ensure that we operate
  // with the correct title context since save copying requires title directories to exist.
  Common::ScopeGuard wiifs_guard{[&boot_session_data] {
    Core::CleanUpWiiFileSystemContents(boot_session_data);
    boot_session_data.InvokeWiiSyncCleanup();
  }};
  if (system.IsWii())
    Core::InitializeWiiFileSystemContents(savegame_redirect, boot_session_data);
  else
    wiifs_guard.Dismiss();

  // This adds the SyncGPU handler to CoreTiming, so now CoreTiming::Advance might block.
  system.GetFifo().Prepare();

  const Common::EventHook frame_presented =
      GetVideoEvents().after_present_event.Register(&Core::Callback_FramePresented);

  // Setup our core
  if (Config::Get(Config::MAIN_CPU_CORE) != PowerPC::CPUCore::Interpreter)
  {
    system.GetPowerPC().SetMode(PowerPC::CoreMode::JIT);
  }
  else
  {
    system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);
  }

  UpdateTitle(system);

  // Become the CPU thread.
  cpu_thread_func(system, savestate_path, delete_savestate);
}

// Set or get the running state

void SetState(Core::System& system, State state, bool report_state_change,
              bool override_achievement_restrictions)
{
  {
    std::lock_guard lock(s_core_mutex);

    // State cannot be controlled until the CPU Thread is operational
    if (s_state.load() != State::Running)
      return;

    switch (state)
    {
    case State::Paused:
#ifdef USE_RETRO_ACHIEVEMENTS
      if (!override_achievement_restrictions && !AchievementManager::GetInstance().CanPause())
        return;
#endif  // USE_RETRO_ACHIEVEMENTS
      // NOTE: GetState() will return State::Paused immediately, even before anything has
      //   stopped (including the CPU).
      system.GetCPU().SetStepping(true);  // Break
      Wiimote::Pause();
      ResetRumble();
#ifdef USE_RETRO_ACHIEVEMENTS
      AchievementManager::GetInstance().DoIdle();
#endif  // USE_RETRO_ACHIEVEMENTS
      break;
    case State::Running:
    {
      system.GetCPU().SetStepping(false);
      Wiimote::Resume();
      break;
    }
    default:
      PanicAlertFmt("Invalid state");
      break;
    }
  }

  // Certain callers only change the state momentarily. Sending a callback for them causes
  // unwanted updates, such as the Pause/Play button flickering between states on frame advance.
  if (report_state_change)
    NotifyStateChanged(GetState(system));
}

State GetState(Core::System& system)
{
  const State state = s_state.load();
  if (state == State::Running && system.GetCPU().IsStepping())
    return State::Paused;
  else
    return state;
}


bool PauseAndLock(Core::System& system)
{
  s_core_mutex.lock();

  if (!IsRunning(system))
    return true;

  // First pause the CPU.  This acquires a wrapper mutex and converts the current thread into
  // a temporary replacement CPU Thread.
  const bool was_unpaused = system.GetCPU().PauseAndLock();

  // audio has to come after CPU, because CPU thread can wait for audio thread (m_throttle).
  system.GetDSP().GetDSPEmulator()->PauseAndLock();

  // video has to come after CPU, because CPU thread can wait for video thread
  // (s_efbAccessRequested).
  system.GetFifo().PauseAndLock();

  ResetRumble();

  return was_unpaused;
}

void RestoreStateAndUnlock(Core::System& system, const bool unpause_on_unlock)
{
  Common::ScopeGuard scope_guard([] { s_core_mutex.unlock(); });

  if (!IsRunning(system))
    return;

  system.GetDSP().GetDSPEmulator()->UnpauseAndUnlock();
  ResetRumble();

  // CPU is unlocked last because CPU::RestoreStateAndUnlock contains the synchronization mechanism
  // that prevents CPU::Break from racing.
  //
  // The CPU is responsible for managing the Audio and FIFO state so we use its mechanism to unpause
  // them. If we unpaused the systems above when releasing the locks then they could call CPU::Break
  // which would require detecting it and re-pausing with CPU::SetStepping.
  system.GetCPU().RestoreStateAndUnlock(unpause_on_unlock);
}

void RunOnCPUThread(Core::System& system, Common::MoveOnlyFunction<void()> function)
{
  if (IsCPUThread())
  {
    function();
    return;
  }

  // Pause the CPU (set it to stepping mode).
  const bool was_running = PauseAndLock(system);

  if (!IsRunning(system))
  {
    // If the core hasn't been started, there is no active CPU thread we can race against.
    function();
  }
  else
  {
    // Queue the job function.
    system.GetCPU().AddCPUThreadJob(std::move(function));
  }

  // Release the CPU thread, and let it execute the callback.
  RestoreStateAndUnlock(system, was_running);
}

// --- Callbacks for backends / engine ---

// Called from Renderer::Swap (GPU thread) when a frame is presented to the host screen.
void Callback_FramePresented(const PresentInfo& present_info)
{
  auto& perf_metrics = Core::System::GetInstance().GetPerfMetrics();
  perf_metrics.CountFrame();

  const auto presentation_offset =
      present_info.actual_present_time - present_info.intended_present_time;
  perf_metrics.SetLatestFramePresentationOffset(presentation_offset);

  if (present_info.reason == PresentInfo::PresentReason::VideoInterfaceDuplicate)
    return;

  s_stop_frame_step.store(true);
}

// Called from VideoInterface::Update (CPU thread) at emulated field boundaries
void Callback_NewField(Core::System& system)
{
  if (s_frame_step)
  {
    // To ensure that s_stop_frame_step is up to date, wait for the GPU thread queue to empty,
    // since it is may contain a swap event (which will call Callback_FramePresented). This hurts
    // the performance a little, but luckily, performance matters less when using frame stepping.
    AsyncRequests::GetInstance()->WaitForEmptyQueue();

    // Only stop the frame stepping if a new frame was displayed
    // (as opposed to the previous frame being displayed for another frame).
    if (s_stop_frame_step.load())
    {
      s_frame_step = false;
      system.GetCPU().Break();
      NotifyStateChanged(Core::GetState(system));
    }
  }

  AchievementManager::GetInstance().DoFrame();
}

void UpdateTitle(Core::System& system)
{
  // Settings are shown the same for both extended and summary info
  const std::string SSettings = fmt::format(
      "{} {} | {} | {}", system.GetPowerPC().GetCPUName(), system.IsDualCoreMode() ? "DC" : "SC",
      g_video_backend->GetDisplayName(), Config::Get(Config::MAIN_DSP_HLE) ? "HLE" : "LLE");

  std::string message = fmt::format("{} | {}", Common::GetScmRevStr(), SSettings);
  if (Config::Get(Config::MAIN_SHOW_ACTIVE_TITLE))
  {
    const std::string& title = SConfig::GetInstance().GetTitleDescription();
    if (!title.empty())
      message += " | " + title;
  }

  Host_UpdateTitle(message);
}

void Shutdown(Core::System& system)
{
  // During shutdown DXGI expects us to handle some messages on the UI thread.
  // Therefore we can't immediately block and wait for the emu thread to shut
  // down, so we join the emu thread as late as possible when the UI has already
  // shut down.
  // For more info read "DirectX Graphics Infrastructure (DXGI): Best Practices"
  // on MSDN.
  if (s_emu_thread.joinable())
    s_emu_thread.join();

  // Make sure there's nothing left over in case we're about to exit.
  HostDispatchJobs(system);
}

Common::EventHook AddOnStateChangedCallback(StateChangedCallbackFunc callback)
{
  return s_state_changed_event.Register(std::move(callback));
}

void NotifyStateChanged(const Core::State state)
{
  s_state_changed_event.Trigger(state);
}

void UpdateWantDeterminism(Core::System& system, bool initial)
{
  const Core::CPUThreadGuard guard(system);

  // For now, this value is not itself configurable.  Instead, individual
  // settings that depend on it, such as GPU determinism mode. should have
  // override options for testing,
  // The harness is a third reason to want determinism, alongside movies and
  // netplay -- and it has to go through the same switch, because this also
  // drives FIFO determinism and JIT FMA use, either of which would otherwise
  // make the measurement disagree with the netplay it is meant to predict.
  // An as-played harness run (RecompDeterminism::IsAsPlayed) opts out: it measures the player's shape.
  bool new_want_determinism =
      system.GetMovie().IsMovieActive() || NetPlay::IsNetPlayRunning() ||
      (RecompDeterminism::IsActive() && !RecompDeterminism::IsAsPlayed());
  if (new_want_determinism != s_wants_determinism || initial)
  {
    NOTICE_LOG_FMT(COMMON, "Want determinism <- {}", new_want_determinism ? "true" : "false");

    s_wants_determinism = new_want_determinism;
    const auto ios = system.GetIOS();
    if (ios)
      ios->UpdateWantDeterminism(new_want_determinism);

    system.GetFifo().UpdateWantDeterminism(new_want_determinism);

    // We need to clear the cache because some parts of the JIT depend on want_determinism,
    // e.g. use of FMA.
    system.GetJitInterface().ClearCache(guard);
  }
}



// NOTE: Host Thread
void DoFrameStep(Core::System& system)
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    OSD::AddMessage("Frame stepping is disabled in RetroAchievements hardcore mode");
    return;
  }

  std::lock_guard lock(s_core_mutex);

  if (GetState(system) == State::Paused)
  {
    // if already paused, frame advance for 1 frame
    s_stop_frame_step = false;
    s_frame_step = true;
    SetState(system, State::Running, false);
  }
  else if (!s_frame_step)
  {
    // if not paused yet, pause immediately instead
    SetState(system, State::Paused);
  }
}

void UpdateInputGate(bool require_focus, bool require_full_focus)
{
  // If the user accepts background input, controls should pass even if an on screen interface is on
  const bool focus_passes =
      !require_focus ||
      ((Host_RendererHasFocus() || Host_TASInputHasFocus()) && !Host_UIBlocksControllerState());
  // Ignore full focus if we don't require basic focus
  const bool full_focus_passes =
      !require_focus || !require_full_focus || (focus_passes && Host_RendererHasFullFocus());
  ControlReference::SetInputGate(focus_passes && full_focus_passes);
}



}  // namespace Core
