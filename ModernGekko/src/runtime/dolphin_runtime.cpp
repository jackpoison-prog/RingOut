#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Common/MsgHandler.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GCPad.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/RecompDeterminism.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/RecompMenu.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>

#ifndef MODERNGEKKO_PROJECT_NAME
#define MODERNGEKKO_PROJECT_NAME "Ring Out"
#endif
#ifndef MODERNGEKKO_PROJECT_VERSION
#define MODERNGEKKO_PROJECT_VERSION "Ver 1.0"
#endif

// Process-wide state shared between the Host_* callbacks Dolphin calls from its
// own threads and the single live Runtime. s_runtime_mutex guards creation and
// teardown; the title fields are only written while it is held.
namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));

std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;

// Net-wait telemetry decoration removed: NetPlay::InputWaitTelemetry /
// GetInputWaitTelemetry live only in an unpushed RecompCore fork. The title is
// just "<title> | <fps> FPS", in netplay as well as single player.
std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  return fmt::format("{} | {:.1f} FPS", title, fps);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  auto &perf = Core::System::GetInstance().GetPerfMetrics();
  static const bool s_log_speed = std::getenv("STATICRECOMP_SPEED") != nullptr;
  if (s_log_speed)
    std::fprintf(stderr, "[perf] speed=%.1f%% fps=%.1f vps=%.1f\n",
                 perf.GetSpeed() * 100.0, perf.GetFPS(), perf.GetVPS());

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(title, perf.GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
};

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

namespace {
// A pad profile names its device as a string ("SDL/0/Steam Deck Controller").
// That string is not stable: on a Steam Deck the same physical pad enumerates as
// "SDL/0/Steam Deck Controller" or "SDL/0/Steam Virtual Gamepad" depending on
// whether Steam Input is in play, and Dolphin resolves a profile naming an
// absent device to nothing at all -- every binding evaluates empty, so the game
// sees a centred, unpressed controller and NOTHING reports an error. That is
// what made a Deck's controller work in single player and do nothing in netplay.
//
// So: if the configured device is gone but a real gamepad is present, move the
// profile onto it. Only when the configured one is genuinely absent, so an
// explicit choice is never overridden while it still exists.
void RebindPadsToPresentDevices() {
  InputConfig *const config = Pad::GetConfig();
  if (config == nullptr)
    return;

  const std::vector<std::string> available =
      g_controller_interface.GetAllDeviceStrings();
  // Prefer a real gamepad over the keyboard/pointer pair, which is always
  // present and would otherwise look like a valid answer.
  const auto gamepad = std::ranges::find_if(
      available, [](const std::string &d) { return d.starts_with("SDL/"); });

  for (int i = 0; i < config->GetControllerCount(); ++i) {
    auto *const pad = config->GetController(i);
    if (pad == nullptr)
      continue;
    const ciface::Core::DeviceQualifier &current = pad->GetDefaultDevice();
    if (current.ToString().empty())
      continue; // port was never configured; nothing to repair
    if (g_controller_interface.FindDevice(current) != nullptr)
      continue; // still there; leave it alone
    if (gamepad == available.end()) {
      std::fprintf(stderr,
                   "[input] pad %d is bound to '%s', which is not connected, "
                   "and no gamepad is available to move it to\n",
                   i + 1, current.ToString().c_str());
      continue;
    }
    std::fprintf(stderr,
                 "[input] pad %d: '%s' is not connected; rebinding to '%s'\n",
                 i + 1, current.ToString().c_str(), gamepad->c_str());
    pad->SetDefaultDevice(*gamepad);
    pad->UpdateReferences(g_controller_interface);
  }
}

// Checks the configured module against this build's CPU ABI and the disc it was
// generated for. A module that fails validation is fatal unless the caller
// opted into the interpreter, in which case it is dropped from the config.
std::optional<RuntimeError> ResolveModuleSource(RuntimeConfig &config,
                                                const GameMetadata &metadata) {
  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      metadata.disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return RuntimeError{
        RuntimeErrorCode::ModuleRequired,
        "no native module was supplied; use allow_interpreter explicitly"};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)};
    }
    config.module = {};
  }
  validation_library.Close();
  return {};
}

void InitializeUICommon(const std::filesystem::path &user_directory) {
  UICommon::SetUserDirectory(user_directory.string());
  // Only DolphinQt's main() called this, so running headless/NoGUI left the
  // user directory without StateSaves/, Screenshots/, Logs/, Maps/ etc. and
  // anything writing there failed with "failed to create file" -- savestates
  // in particular.
  UICommon::CreateDirectories();
  UICommon::Init();
  // Dolphin's default non-Windows alert handler answers "No" to every
  // question, and ASSERT's PanicYesNo treats "No" as "don't ignore" ->
  // Crash(). A GFX FIFO hiccup then kills the whole game (seen live: dual
  // core desync mid-session, SIGILL). There is no UI to ask, so log the
  // alert and always pick the continue path.
  Common::RegisterMsgAlertHandler([](const char *caption, const char *text,
                                     bool yes_no, Common::MsgType style) -> bool {
    std::fprintf(stderr, "[alert] %s: %s\n", caption, text);
    return true;
  });
}

std::unique_ptr<Platform> CreateHostPlatform(const RuntimeConfig &config) {
  if (config.headless)
    return Platform::CreateHeadlessPlatform();
#ifdef MODERNGEKKO_HAVE_COCOA
  return Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  if (config.window_system != WindowSystem::Wayland)
    return Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  if (config.window_system != WindowSystem::X11)
    return Platform::CreateWaylandPlatform();
#endif
  return nullptr;
}

void ApplyCoreSettings(const GameMetadata &metadata) {
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  // Dolphin defaults CPUThread=false (single-core) on desktop, which runs the
  // GPU synchronously on the CPU thread — every full-screen XFB blit / texture
  // upload (heavy during FMV) then stalls the recompiled core. Dual-core moves
  // the GPU to its own thread (matching real GC's async GP), the biggest perf
  // win for FMV/gameplay. The recomp core drives the FIFO like any CPU core, so
  // this is orthogonal to StaticRecomp.
  // ...except when hashing state per frame. Dual-core has the GPU thread
  // writing guest RAM asynchronously, so RAM read at a CPU frame boundary is
  // racy by construction and two runs would differ whether or not the core is
  // deterministic. The determinism harness therefore needs single-core, or it
  // measures its own noise.
  //
  // RINGOUT_DETERMINISM_DUALCORE=1 lifts that, so the harness can measure the
  // configuration netplay actually ships (dual-core + a deterministic GPU
  // thread) instead of one it does not. It is only sound alongside the quiesce
  // in RecompDeterminism::OnFrame -- without that the hashes race and the run
  // measures its own noise, which is the trap this whole comment is about. Left
  // opt-in so every previously verified result keeps its exact shape.
  const bool determinism_dual_core =
      RecompDeterminism::IsActive() &&
      std::getenv("RINGOUT_DETERMINISM_DUALCORE") != nullptr;
  Config::SetBase(Config::MAIN_CPU_THREAD,
                  !RecompDeterminism::IsActive() || determinism_dual_core);
  if (determinism_dual_core)
    Config::SetBase(Config::MAIN_GPU_DETERMINISM_MODE,
                    std::string("fake-completion"));
  if (RecompDeterminism::IsActive()) {
    // Pin the clock the same way NetPlayServer does (NetPlayServer.cpp:2088):
    // the RTC is converted to timebase ticks at boot, so two runs started
    // seconds apart diverge from frame 0 through every value the game seeds
    // from it. Forced here rather than left to the ini, because the setting is
    // spelled EnableCustomRTC and getting that wrong fails silently -- which is
    // exactly what happened to the first attempt at this measurement.
    Config::SetBase(Config::MAIN_CUSTOM_RTC_ENABLE, true);
    Config::SetBase(Config::MAIN_CUSTOM_RTC_VALUE, 0x386D4380u);
  }
  // The OS scheduler spins in an idle loop waiting for an interrupt to wake a
  // task. Without idle-skip the recomp core burns real wall-time executing that
  // spin, which halved FMV / gameplay speed (movies ran in slow-motion).
  // Pointing the core's idle-skip at that PC makes CoreTiming fast-forward to
  // the next event instead → full 60fps. (Idle-skip is the standard Dolphin
  // approach; only the PC is game-specific.)
  //
  // The address is per-build, so InspectGame finds the loop in the DOL rather
  // than this being a constant per disc ID -- which is what lets a disc from
  // another region run at full speed without a new constant here. It resolves
  // to 0x80185DEC on GRSEAF (the value this was, hand-found) and 0x8017F35C on
  // GRSJAF. GRSEPS, the "SC2 Plus" community mod, appends its own code at
  // 0x80476000 and hooks the base text in place rather than relocating it, so
  // its scheduler and idle loop stay where GRSEAF's are and it detects to the
  // same address.
  //
  // Left unset when no single loop matches: idle-skip then stays off, exactly
  // as it was for every disc but this one before.
  if (metadata.idle_pc)
    Config::SetBase(Config::MAIN_STATICRECOMP_IDLE_PC, *metadata.idle_pc);
}

void ApplyGraphicsSettings(const GraphicsSettings &graphics, bool headless) {
  if (!graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, graphics.backend);
  else if (headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE, *graphics.internal_resolution_scale);
  // SoulCalibur II and most GC titles render a 4:3 projection. ForceWide alone
  // would just stretch that image; the widescreen hack widens the projection
  // matrix so the extra horizontal field of view is actually drawn. The pair is
  // set together — either both on (16:9) or both off (native 4:3). Alt+W flips
  // them at runtime via Config::SetCurrent (VideoConfig::Refresh picks it up on
  // the next frame).
  // Only forced when --widescreen is passed; otherwise the value saved by the
  // in-game menu (Alt+W / Settings) carries over between launches.
  if (graphics.widescreen) {
    Config::SetBase(Config::GFX_WIDESCREEN_HACK, *graphics.widescreen);
    Config::SetBase(Config::GFX_ASPECT_RATIO, *graphics.widescreen
                                                  ? AspectMode::ForceWide
                                                  : AspectMode::Auto);
  }
  Config::SetBase(Config::GFX_SHADER_CACHE, true);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
}

// Resolves audio.backend to something this host actually offers, writing the
// choice back so GetConfig() reports what is really in use.
void ApplyAudioSettings(AudioSettings &audio, bool headless) {
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (headless) {
    audio.backend = BACKEND_NULLSOUND;
  } else if (audio.backend.empty() ||
             std::ranges::find(audio_backends, audio.backend) ==
                 audio_backends.end()) {
    audio.backend = AudioCommon::GetDefaultSoundBackend();
    if (audio.backend == BACKEND_NULLSOUND) {
      const auto available =
          std::ranges::find_if(audio_backends, [](const std::string &backend) {
            return backend != BACKEND_NULLSOUND;
          });
      if (available != audio_backends.end())
        audio.backend = *available;
    }
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, audio.backend);
}

void ApplyModuleSourceToJit(const ModuleSource &module) {
  auto &jit = Core::System::GetInstance().GetJitInterface();
  if (module.kind == ModuleSource::Kind::DynamicPath)
    jit.SetStaticRecompModuleSource(
        StaticRecompModuleSource::Dynamic(module.path.string()));
  else if (module.kind == ModuleSource::Kind::AttachedDescriptor)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(module.descriptor)));
  else
    jit.SetStaticRecompModuleSource({});
}

// Takes whatever boot session data a host (netplay) staged for this run; the
// slot is one-shot, so it is cleared whether or not anything was there.
std::unique_ptr<BootParameters> TakeBootParameters(const std::string &main_dol) {
  std::lock_guard lock(s_runtime_mutex);
  std::unique_ptr<BootParameters> boot;
  if (s_boot_session_data)
    boot = BootParameters::GenerateFromFile(main_dol,
                                            std::move(*s_boot_session_data));
  else
    boot = BootParameters::GenerateFromFile(main_dol);
  s_boot_session_data.reset();
  return boot;
}

// Dolphin only refreshes the window title from its own Host_UpdateTitle calls,
// which stop while the core is paused, so drive it from here at ~1 Hz. The
// inner loop sleeps in 100 ms slices so a stop request is honoured promptly.
std::jthread StartTitleThread() {
  return std::jthread([](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      Host_UpdateTitle({});
      for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
}
} // namespace

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  if (auto error = ResolveModuleSource(config, *inspected.metadata))
    return {{}, std::move(*error)};

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      std::string(MODERNGEKKO_PROJECT_NAME) + " " + MODERNGEKKO_PROJECT_VERSION);

  if (!s_external_ui_common) {
    InitializeUICommon(impl->config.user_directory);
    impl->ui_initialized = true;
  }

  impl->platform = CreateHostPlatform(impl->config);
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

  UICommon::InitControllers(impl->platform->GetWindowSystemInfo());
  impl->controllers_initialized = true;
  RebindPadsToPresentDevices();
  impl->platform->SetTitle(impl->title);

  ApplyCoreSettings(impl->metadata);
  ApplyGraphicsSettings(impl->config.graphics, impl->config.headless);
  ApplyAudioSettings(impl->config.audio, impl->config.headless);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                  impl->config.input.background_input);
  ApplyModuleSourceToJit(impl->config.module);

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot =
      TakeBootParameters(m_impl->metadata.main_dol.string());
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  // Continue-where-you-left-off (menu System > Auto-Resume). Headless runs are
  // benchmarks/verification; loading a state there would corrupt them.
  if (!m_impl->config.headless)
    RecompMenu::ScheduleAutoResumeLoad();
  std::jthread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title)
    title_thread = StartTitleThread();
  m_impl->platform->MainLoop();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko
