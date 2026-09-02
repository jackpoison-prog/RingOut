// netplay_session.cpp — headless netplay lobby.
//
// The previous implementation was written against a private RecompCore fork of
// NetPlayClient/NetPlayServer (SetReady, CanStart, SetLocalControllerCount,
// GetPlayersSnapshot, GetConnectionError, SetAdaptiveBuffer, ...) that is not in
// the vendored Dolphin, so it could not be compiled here and was replaced by a
// stub that always reported "netplay is unavailable". This is a rewrite against
// the vendored API only. The original is kept at
// work/out/netplay_session.cpp.orig; its SessionUI carried over nearly intact,
// while its ImGui/SDL3 lobby window is gone -- this runs headless and is driven
// by flags, which is what a scripted two-instance test needs.
//
// Differences forced by the vendored API, all of them deliberate:
//
//   * There is no ready protocol. The fork had per-player SetReady/CanStart;
//     upstream Dolphin has neither, and the host simply starts. So the host
//     waits for --netplay-players machines to connect and for every one of them
//     to report it has the game, then starts.
//   * Pads must be assigned explicitly. NetPlayServer initialises its map with
//     m_pad_map.fill(0), and 0 means "unassigned" (player ids start at 1), so
//     without this nobody's controller reaches the game and both sides sit at a
//     dead title screen looking like a sync failure.
//   * The client exposes no GetConnectionError(), so the failure reason is
//     captured from the OnConnectionError callback instead.

#include "netplay_session.hpp"
#include <ctime>
#include <fstream>

#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/SI/SI_Device.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"
#include "Core/HW/GCPad.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "VideoCommon/RecompGameData.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"
#include "netplay_compatibility.hpp"
#include "runtime/dolphin_runtime_internal.hpp"

#include <SDL3/SDL.h>
#include <enet/enet.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace moderngekko::frontend {
namespace {

using Clock = std::chrono::steady_clock;

// Netplay writes to a FILE as well as stderr. A player who launched from Steam
// -- which is how the Deck package is meant to be run -- has no terminal, so
// every explanation this session produces was going nowhere. Issue #2 arrived
// as a PHOTOGRAPH of a dialog because that was the only way to report what
// happened.
std::mutex g_log_mutex;
std::ofstream g_log_file;
std::filesystem::path g_log_path;

void OpenSessionLog(const std::filesystem::path &user_directory) {
  std::error_code ec;
  const auto dir = user_directory / "Logs";
  std::filesystem::create_directories(dir, ec);
  std::lock_guard<std::mutex> guard(g_log_mutex);
  // Appended, not truncated: "it failed three times and here is each one" is
  // the report worth having, and a session that dies early would otherwise
  // erase the one before it.
  g_log_file.open(dir / "netplay.log", std::ios::app);
  g_log_path = dir / "netplay.log";
  if (g_log_file) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    g_log_file << "\n==== netplay session " << stamp << " ====\n";
    g_log_file.flush();
  }
}

const std::filesystem::path &SessionLogPath() { return g_log_path; }

void Log(const std::string &message) {
  std::cerr << "netplay: " << message << '\n';
  std::cerr.flush();
  std::lock_guard<std::mutex> guard(g_log_mutex);
  if (g_log_file) {
    g_log_file << message << '\n';
    g_log_file.flush();   // flushed per line: a crash must not eat the reason
  }
}

// Bridges Dolphin's netplay callbacks to a headless session. Every method is
// called from the netplay thread, so shared fields are mutex- or atomic-guarded
// and the lobby loop only polls them.
class SessionUI final : public NetPlay::NetPlayUI {
public:
  explicit SessionUI(std::shared_ptr<UICommon::GameFile> game)
      : m_game(std::move(game)) {}

  void SetHosting(bool hosting) { m_hosting = hosting; }
  void SetRuntime(Runtime *runtime) {
    std::lock_guard lock(m_mutex);
    m_runtime = runtime;
  }

  bool TakeStartRequest() { return m_start_requested.exchange(false); }

  std::unique_ptr<BootSessionData> TakeBootData() {
    std::lock_guard lock(m_mutex);
    return std::move(m_boot_data);
  }

  std::string Error() {
    std::lock_guard lock(m_mutex);
    return m_error;
  }

  bool ConnectionLost() const { return m_connection_lost.load(); }
  bool Desynced() const { return m_desynced.load(); }
  u32 DesyncFrame() const { return m_desync_frame.load(); }

  // --- NetPlayUI -----------------------------------------------------------
  // Dolphin hands us the boot data instead of booting itself; the lobby picks
  // it up and feeds it to the runtime through detail::SetBootSessionData.
  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData> boot_session_data) override {
    std::lock_guard lock(m_mutex);
    m_boot_data = std::move(boot_session_data);
  }

  void StopGame() override {
    std::lock_guard lock(m_mutex);
    if (m_runtime)
      m_runtime->RequestStop();
  }

  bool IsHosting() const override { return m_hosting; }
  void Update() override {}

  void AppendChat(const std::string &msg) override { Log("chat: " + msg); }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier &sync_identifier,
                       const std::string &name) override {
    // With the disc ID, not just the name. All three regions of this game call
    // themselves SOULCALIBUR2 internally, so the name alone reads identically
    // whether the host is on GRSEAF, GRSJAF or GRSPAF -- which is exactly the
    // case a player needs to see, since those cannot play together.
    Log("game selected: " + name + " (" + sync_identifier.game_id + ")");
  }

  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}

  void OnMsgStartGame() override {
    Log("start signalled by host");
    m_start_requested = true;
  }

  void OnMsgStopGame() override { StopGame(); }
  void OnMsgPowerButton() override { StopGame(); }

  void OnPlayerConnect(const std::string &player) override {
    Log("player joined: " + player);
  }

  void OnPlayerDisconnect(const std::string &player) override {
    Log("player left: " + player);
  }

  void OnPadBufferChanged(u32 buffer) override {
    Log("pad buffer = " + std::to_string(buffer) + " frames");
  }

  void OnHostInputAuthorityChanged(bool) override {}

  // The whole point of the determinism work: if the two cores ever disagree,
  // Dolphin says so here, with the frame it happened on.
  void OnDesync(u32 frame, const std::string &player) override {
    m_desynced = true;
    m_desync_frame = frame;
    {
      std::lock_guard lock(m_mutex);
      m_error = "desync at frame " + std::to_string(frame);
      if (!player.empty())
        m_error += " reported for " + player;
    }
    Log("DESYNC at frame " + std::to_string(frame) +
        (player.empty() ? std::string() : " (" + player + ")"));
    StopGame();
  }

  void OnConnectionLost() override {
    m_connection_lost = true;
    {
      std::lock_guard lock(m_mutex);
      m_error = "connection to the host was lost";
    }
    Log("connection lost");
    StopGame();
  }

  void OnConnectionError(const std::string &message) override {
    {
      std::lock_guard lock(m_mutex);
      m_error = message;
    }
    Log("connection error: " + message);
  }

  void OnTraversalError(Common::TraversalClient::FailureReason) override {
    OnConnectionError("direct connection failed");
  }

  void OnTraversalStateChanged(Common::TraversalClient::State) override {}

  void OnGameStartAborted() override {
    {
      std::lock_guard lock(m_mutex);
      m_error = "game start was aborted";
    }
    Log("game start aborted");
  }

  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }

  // One game per package, so every sync identifier resolves to it; the
  // comparison result is what tells the peer whether we match.
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &sync_identifier,
               NetPlay::SyncIdentifierComparison *found) override {
    const auto comparison = m_game->CompareSyncIdentifier(sync_identifier);
    if (found)
      *found = comparison;
    return m_game;
  }

  std::string FindGBARomPath(const std::array<u8, 20> &, std::string_view,
                             int) override {
    return {};
  }

  void ShowGameDigestDialog(const std::string &) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string &) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}

  void ShowChunkedProgressDialog(const std::string &title, u64,
                                 std::span<const int>) override {
    Log("transfer: " + title);
  }

  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}

private:
  std::shared_ptr<UICommon::GameFile> m_game;
  mutable std::mutex m_mutex;
  Runtime *m_runtime = nullptr;
  std::atomic<bool> m_hosting{false};
  std::atomic<bool> m_start_requested{false};
  std::atomic<bool> m_connection_lost{false};
  std::atomic<bool> m_desynced{false};
  std::atomic<u32> m_desync_frame{0};
  std::unique_ptr<BootSessionData> m_boot_data;
  std::string m_error;
};

// Give every connected player one controller port, lowest player id first, so
// the host is always pad 1 and the assignment does not depend on join order.
// Without this the map stays all-zero and no input reaches the game.
// What this peer will actually do with its controller. The pad map alone is not
// enough: routing is LocalPadToInGamePad(0), and "no input reaches player 2" and
// "both peers drive player 1" are both answered by that one number. Logged from
// the host and the client paths alike, because only each peer's own view counts.
void LogPadRouting(NetPlay::NetPlayClient &client) {
  const NetPlay::PadMappingArray &m = client.GetPadMapping();
  std::string view;
  for (size_t i = 0; i < m.size(); ++i)
    view += (i ? "," : "") + std::to_string(static_cast<int>(m[i]));
  const int local_pads = std::ranges::count(m, client.GetLocalPlayerId());
  std::string routing;
  for (int lp = 0; lp < local_pads; ++lp)
    routing += " local pad " + std::to_string(lp) + " -> in-game pad " +
               std::to_string(client.LocalPadToInGamePad(lp) + 1);
  Log("pad routing here: map=[" + view + "] my pid=" +
      std::to_string(static_cast<int>(client.GetLocalPlayerId())) +
      " local pads=" + std::to_string(local_pads) +
      (routing.empty() ? std::string("  NONE - this peer sends no input") : routing));
}

void AssignPads(NetPlay::NetPlayServer &server,
                const std::vector<const NetPlay::Player *> &players) {
  std::vector<NetPlay::PlayerId> ids;
  ids.reserve(players.size());
  for (const NetPlay::Player *player : players)
    ids.push_back(player->pid);
  std::sort(ids.begin(), ids.end());

  NetPlay::PadMappingArray mapping{};
  mapping.fill(0);
  for (size_t i = 0; i < ids.size() && i < mapping.size(); ++i)
    mapping[i] = ids[i];
  server.SetPadMapping(mapping);

  std::string summary;
  for (size_t i = 0; i < mapping.size(); ++i) {
    if (mapping[i] == 0)
      continue;
    if (!summary.empty())
      summary += ", ";
    summary += "pad " + std::to_string(i + 1) + " -> player " +
               std::to_string(static_cast<int>(mapping[i]));
  }
  Log("controllers: " + (summary.empty() ? std::string("none") : summary));
}

const char *GameStatusText(NetPlay::SyncIdentifierComparison status) {
  switch (status) {
  case NetPlay::SyncIdentifierComparison::SameGame:
    return "ready";
  case NetPlay::SyncIdentifierComparison::DifferentHash:
    return "different dump";
  case NetPlay::SyncIdentifierComparison::DifferentDiscNumber:
    return "different disc";
  case NetPlay::SyncIdentifierComparison::DifferentRevision:
    return "different revision";
  case NetPlay::SyncIdentifierComparison::DifferentRegion:
    return "different region";
  case NetPlay::SyncIdentifierComparison::DifferentGame:
    return "different game";
  default:
    return "checking ...";
  }
}

// SDL3 + ImGui lobby. The build already compiles the ImGui SDL3 backends into
// this binary (they were there for the fork's lobby), so this needs no new
// dependency.
class LobbyWindow {
public:
  bool Open(WindowSystem window_system) {
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,
                window_system == WindowSystem::Wayland ? "wayland" : "x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO))
      return false;
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    m_window = SDL_CreateWindow("Ring Out — Netplay Lobby",
                                static_cast<int>(760 * scale),
                                static_cast<int>(520 * scale),
                                SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window)
      return false;
    m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_renderer)
      return false;
    SDL_SetRenderVSync(m_renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;   // do not litter the user dir
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer3_Init(m_renderer);
    m_open = true;
    return true;
  }

  ~LobbyWindow() { Close(); }

  void Close() {
    if (!m_open)
      return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(m_renderer);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
    m_renderer = nullptr;
    m_window = nullptr;
    m_open = false;
  }

  bool Frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return false;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
  }

  void Present() {
    ImGui::Render();
    SDL_SetRenderDrawColor(m_renderer, 18, 20, 28, 255);
    SDL_RenderClear(m_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_renderer);
    SDL_RenderPresent(m_renderer);
  }

private:
  SDL_Window *m_window = nullptr;
  SDL_Renderer *m_renderer = nullptr;
  bool m_open = false;
};

enum class PortErrorChoice {
  Quit,
  Retry,
  Join,
};

// A busy port is the one failure that is both common and recoverable, and
// locally it almost always means "the other instance is already hosting" --
// which makes joining it the thing the user actually wanted. Exiting the
// process instead, with the reason on a stderr nobody is reading, is a dead end
// from a menu-launched session: the game is already torn down, so there is
// nothing to fall back to.
// Shows WHY a connection failed, on screen, and says where the log is.
//
// Before this, a rejected connection closed the lobby and left nothing behind:
// the reason went to stderr, which a player launching from Steam never sees.
// That is the difference between "it doesn't work" and "the host is on a
// different disc" -- and the second one a player can act on.
void ShowConnectError(WindowSystem window_system, const std::string &reason,
                      const std::filesystem::path &log_path) {
  LobbyWindow window;
  if (!window.Open(window_system))
    return;   // headless or no display: the caller has already logged it

  const auto deadline = Clock::now() + std::chrono::seconds(30);
  while (Clock::now() < deadline) {
    if (!window.Frame())
      break;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Netplay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "Could not join the session.");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", reason.c_str());
    ImGui::Spacing();
    if (!log_path.empty())
      ImGui::TextDisabled("Details: %s", log_path.string().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(140, 40)))
      break;
    ImGui::End();
    window.Present();
  }
  window.Close();
}

PortErrorChoice ShowPortError(WindowSystem window_system, std::uint16_t port,
                              const std::string &address) {
  LobbyWindow window;
  if (!window.Open(window_system))
    return PortErrorChoice::Quit;   // headless or no display: caller logs and exits

  while (true) {
    if (!window.Frame()) {
      window.Close();
      return PortErrorChoice::Quit;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Netplay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                       "Could not host on port %u.", unsigned{port});
    ImGui::TextWrapped(
        "Something else is already using it -- most often another copy of the "
        "game already hosting on this machine.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    PortErrorChoice choice = PortErrorChoice::Quit;
    bool chosen = false;
    if (ImGui::Button("Join that host instead", ImVec2(230, 40))) {
      choice = PortErrorChoice::Join;
      chosen = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Try again", ImVec2(140, 40))) {
      choice = PortErrorChoice::Retry;
      chosen = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(110, 40))) {
      choice = PortErrorChoice::Quit;
      chosen = true;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Joining connects to %s:%u.", address.c_str(),
                        unsigned{port});
    ImGui::End();
    window.Present();
    if (chosen) {
      window.Close();
      return choice;
    }
  }
}

// Poll until the predicate holds, the connection dies, or we run out of
// patience. Headless runs must never block forever: a hung lobby in a script is
// indistinguishable from a slow one.
template <typename Predicate>
bool WaitFor(SessionUI &ui, NetPlay::NetPlayClient &client,
             std::chrono::seconds timeout, Predicate predicate) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (ui.ConnectionLost() || !client.IsConnected())
      return false;
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// Interactive lobby. Returns true when the game should boot, false to quit.
//
// The host assigns pads automatically whenever the player set changes, rather
// than offering a drag-and-drop mapping UI: the map starts all-zero and an
// unassigned player sends no input at all, so a sensible default matters far
// more than the ability to rearrange it.
// LAN discovery beacon, host side.
//
// Netplay here is a direct connection -- no traversal server, no index -- so a
// joiner otherwise has to be told the host's address out of band and type it in
// an octet at a time. A host sitting in the lobby is doing nothing else, so it
// announces itself once a second and the joiner's Scan row picks it up.
//
// ENet's socket wrapper rather than raw BSD sockets because this file and the
// menu both compile on Windows, and enet is already a dependency of both.
//
// The magic string, the port and the payload layout are duplicated in
// RecompMenu.cpp's scanner -- VideoCommon cannot include a header from tools/.
// Change one and the other stops seeing hosts.
constexpr enet_uint16 kDiscoveryPort = 2627;
constexpr const char *kDiscoveryMagic = "RINGOUT1";

class DiscoveryBeacon {
public:
  // Broadcast is not routed, so this reaches the local subnet only. That is the
  // intended scope: over Tailscale or the internet there is no broadcast domain
  // to find anyone on, and the address has to be typed or already saved.
  void Start(std::uint16_t netplay_port, const std::string &nickname) {
    m_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (m_socket == ENET_SOCKET_NULL)
      return;
    if (enet_socket_set_option(m_socket, ENET_SOCKOPT_BROADCAST, 1) != 0) {
      enet_socket_destroy(m_socket);
      m_socket = ENET_SOCKET_NULL;
      return;
    }
    m_payload = std::string(kDiscoveryMagic) + ' ' +
                std::to_string(netplay_port) + ' ' + nickname;
  }

  void Tick() {
    if (m_socket == ENET_SOCKET_NULL)
      return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last < std::chrono::seconds(1))
      return;
    m_last = now;

    // BOTH destinations, and the loopback one is not redundant: Linux does not
    // deliver a broadcast sent out a physical interface back to sockets on the
    // sending machine, so 255.255.255.255 alone means a joiner on the SAME
    // machine never sees the host. Two instances on one box is a supported
    // setup here -- it is what netplay-local.sh does -- and it is also the only
    // way to test discovery without a second PC.
    //   255.255.255.255 -> the LAN, via whichever interface holds the route
    //   127.255.255.255 -> this machine
    //
    // ENetAddress::host is an in_addr, i.e. NETWORK byte order, so the
    // loopback broadcast is built with htonl rather than written as a literal
    // -- 0x7FFFFFFF spelled directly would arrive as 255.255.255.127 on a
    // little-endian machine.
    const enet_uint32 loopback_broadcast = ENET_HOST_TO_NET_32(0x7FFFFFFFU);
    for (const enet_uint32 host : {ENET_HOST_BROADCAST, loopback_broadcast}) {
      ENetAddress address{};
      address.host = host;
      address.port = kDiscoveryPort;
      ENetBuffer buffer{};
      buffer.data = const_cast<char *>(m_payload.data());
      buffer.dataLength = m_payload.size();
      enet_socket_send(m_socket, &address, &buffer, 1);
    }
  }

  ~DiscoveryBeacon() {
    if (m_socket != ENET_SOCKET_NULL)
      enet_socket_destroy(m_socket);
  }

private:
  ENetSocket m_socket = ENET_SOCKET_NULL;
  std::string m_payload;
  std::chrono::steady_clock::time_point m_last{};
};

bool RunLobbyWindow(RuntimeConfig &runtime_config, NetplayOptions &options,
                    SessionUI &ui, NetPlay::NetPlayClient &client,
                    NetPlay::NetPlayServer *server,
                    const std::string &boot_path) {
  LobbyWindow window;
  if (!window.Open(runtime_config.window_system)) {
    Log("could not open the lobby window; falling back to auto-start");
    return true;
  }

  // Only a host is findable; a joiner has nothing to announce.
  DiscoveryBeacon beacon;
  if (server != nullptr)
    beacon.Start(options.port, options.nickname);

  size_t last_player_count = 0;
  unsigned buffer = 5;
  if (options.buffer != "auto") {
    try {
      buffer = static_cast<unsigned>(std::stoul(options.buffer));
    } catch (const std::exception &) {
    }
  }

  while (true) {
    if (!window.Frame())
      return false;
    beacon.Tick();   // rate-limits itself to once a second
    if (ui.ConnectionLost() || !client.IsConnected()) {
      window.Close();
      Log("connection lost while in the lobby");
      return false;
    }

    const std::vector<const NetPlay::Player *> players = client.GetPlayers();

    // Reassign pads when somebody joins or leaves.
    if (server && players.size() != last_player_count) {
      last_player_count = players.size();
      AssignPads(*server, players);
    }

    // OnMsgStartGame only signals; StartGame() is what arms netplay and
    // produces the boot data.
    if (ui.TakeStartRequest())
      client.StartGame(boot_path);
    if (auto boot_data = ui.TakeBootData()) {
      detail::SetBootSessionData(std::move(boot_data));
      window.Close();
      return true;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Ring Out Netplay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);

    if (server)
      ImGui::Text("Hosting on port %u", unsigned{server->GetPort()});
    else
      ImGui::Text("Connected to %s:%u", options.address.c_str(),
                  unsigned{options.port});
    ImGui::Separator();

    const NetPlay::PadMappingArray &map = client.GetPadMapping();
    if (ImGui::BeginTable("players", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
      ImGui::TableSetupColumn("Player");
      ImGui::TableSetupColumn("Ping");
      ImGui::TableSetupColumn("Controller");
      ImGui::TableSetupColumn("Game");
      ImGui::TableHeadersRow();
      for (const NetPlay::Player *player : players) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool is_local = client.IsLocalPlayer(player->pid);
        ImGui::Text("%s%s%s", player->name.c_str(), is_local ? "  (you)" : "",
                    player->IsHost() ? "  [host]" : "");
        ImGui::TableNextColumn();
        ImGui::Text("%u ms", player->ping);
        ImGui::TableNextColumn();
        int pad = 0;
        for (size_t i = 0; i < map.size(); ++i) {
          if (map[i] == player->pid) {
            pad = static_cast<int>(i) + 1;
            break;
          }
        }
        if (pad)
          ImGui::Text("Port %d", pad);
        else
          ImGui::TextDisabled("none");
        ImGui::TableNextColumn();
        const bool ok =
            player->game_status == NetPlay::SyncIdentifierComparison::SameGame;
        if (ok)
          ImGui::TextUnformatted(GameStatusText(player->game_status));
        else
          ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "%s",
                             GameStatusText(player->game_status));
      }
      ImGui::EndTable();
    }

    ImGui::Spacing();
    if (server) {
      // Input delay in frames. Higher hides more jitter at the cost of
      // responsiveness; this is the delay-based knob, and it is the host's.
      int b = static_cast<int>(buffer);
      ImGui::SetNextItemWidth(220);
      if (ImGui::SliderInt("Input buffer (frames)", &b, 1, 20)) {
        buffer = static_cast<unsigned>(b);
        server->AdjustPadBufferSize(buffer);
      }
      ImGui::TextDisabled("About %d ms of delay at 60 fps.",
                          static_cast<int>(buffer * 1000 / 60));
    } else {
      ImGui::TextDisabled("The host controls the input buffer.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (server) {
      const bool everyone_has_game = client.DoAllPlayersHaveGame();
      const bool enough = players.size() >= 1;
      const bool can_start = everyone_has_game && enough;
      ImGui::BeginDisabled(!can_start);
      if (ImGui::Button("Start game", ImVec2(160, 40)))
        server->RequestStartGame();
      ImGui::EndDisabled();
      if (!everyone_has_game) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "Waiting: not everyone has this game.");
      }
    } else {
      ImGui::TextUnformatted("Waiting for the host to start ...");
    }

    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(110, 40))) {
      window.Close();
      return false;
    }

    const std::string error = ui.Error();
    if (!error.empty()) {
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", error.c_str());
    }

    ImGui::End();
    window.Present();
  }
}

} // namespace

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options) {
  (void)frontend_config;
  if (options.controllers.empty())
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);

  Log("initializing Dolphin services");
  UICommon::SetUserDirectory(runtime_config.user_directory.string());
  // Opened here, before anything can fail: every line below reaches the file.
  OpenSessionLog(runtime_config.user_directory);
  UICommon::Init();
  detail::SetExternalUICommon(true);

  // Netplay used to force single-core, reasoning that a dual-core split lets the
  // CPU and GPU threads interleave differently on each peer. Dolphin's own
  // answer to that problem is not single-core but a DETERMINISTIC GPU thread
  // (Fifo.cpp, UpdateWantDeterminism), which pre-processes the FIFO so the CPU
  // thread's view does not depend on interleaving. Its enabling condition ends
  // with `gpu_thread && IsDualCoreMode()`, so forcing single-core switched off
  // the very mechanism built to make dual-core safe.
  //
  // Measured on the Deck over a real PC-to-Deck match: 41 -> 46 fps (+12%),
  // no desync. Offline was already dual-core, so netplay was the only mode
  // paying for this. Also clean over a ~6.5 min two-peer soak and a shorter run.
  //
  // Dual-core with a deterministic GPU thread. Netplay used to force
  // single-core, but Dolphin's answer to CPU/GPU interleaving is not single-core
  // -- it is the deterministic GPU thread (Fifo.cpp, UpdateWantDeterminism),
  // whose enabling condition ends `gpu_thread && IsDualCoreMode()`. Forcing
  // single-core switched off the very mechanism built to make dual-core safe.
  //
  // Measured on the Deck over a real PC-to-Deck match: 41 -> 46 fps (+12%).
  // Verified byte-identical across two peers over 6083 and 8847 frames with
  // RINGOUT_DETERMINISM_DUALCORE=1, and reproducible run-to-run over 600.
  //
  // This was reverted once: opening the in-game menu mid-session fired
  // "SyncGPUCallback event scheduled from the wrong thread". That is fixed --
  // RunGpu now schedules with FromThread::ANY, since AsyncRequests::QueueEvent
  // calls it from off the CPU thread -- but the original assert was never
  // reproduced locally, so the fix rests on reading the one path that schedules
  // that event. RINGOUT_NETPLAY_SINGLECORE=1 restores the old shape and is the
  // first thing to try if a peer ever asserts or desyncs.
  const bool single_core = std::getenv("RINGOUT_NETPLAY_SINGLECORE") != nullptr;
  Config::SetBase(Config::MAIN_CPU_THREAD, !single_core);
  if (!single_core)
    Config::SetBase(Config::MAIN_GPU_DETERMINISM_MODE, std::string("fake-completion"));
  Log(single_core ? "netplay: single-core (forced)"
                  : "netplay: dual-core with a deterministic GPU thread");
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);

  // Stock clock for netplay, on every peer. Dolphin does sync the host's
  // overclock through its settings layer, so this is not strictly required for
  // agreement -- but the factor rescales CoreTiming's cycle conversion, and the
  // recompiler's cycle accounting (downcount charged at block leaders, the
  // mid-block exception refund) plus every determinism result were established
  // at 1.0. Forcing it here means the host cannot advertise anything else.
  Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, false);
  Config::SetBase(Config::MAIN_OVERCLOCK, 1.0f);
  Config::SetBase(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
  Config::SetBase(Config::MAIN_VI_OVERCLOCK, 1.0f);

  // Dolphin drops all input, pipe included, whenever the render window lacks
  // focus, and it defaults to off. That is wrong for netplay in two ways: a
  // peer that loses focus mid-match silently sends neutral input while its
  // opponent keeps playing, and two peers on one machine can never both be
  // focused -- which presents exactly as "the guest's controller does nothing"
  // while the host's works, with no error anywhere.
  //
  // This has to go on the RuntimeConfig, not through Config::SetBase: writing
  // it into Dolphin.ini does not survive init, and Runtime::Create then does
  // SetBase(MAIN_INPUT_BACKGROUND_INPUT, config.input.background_input)
  // unconditionally, which overwrites anything set here beforehand.
  runtime_config.input.background_input = true;

  auto game = std::make_shared<UICommon::GameFile>(
      (runtime_config.game_root / "sys/main.dol").string());
  if (!game->IsValid()) {
    Log("the game at " + runtime_config.game_root.string() + " is not valid");
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }

  const GameInspectResult inspected = InspectGame(runtime_config.game_root);
  if (!inspected) {
    Log("could not inspect the game: " + inspected.error);
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }

  // Agreed at connect time, before a lobby exists to stall in. Both peers
  // compute this from their own disc and module; the host rejects a peer whose
  // disc ID differs (a JP or PAL copy against a US host) or whose fingerprint
  // does -- same disc, module built by different tools. Without it the mismatch
  // surfaced as a 30-second wait and "no start signal arrived", which told the
  // joining player nothing about what was wrong.
  // Hash the game data HERE and not only in Runtime::Create: the fingerprint is
  // taken at connect time, which is long before the runtime exists. Left to the
  // runtime, both peers would fingerprint an un-hashed "unknown", match each
  // other, and the root.olk field would protect nobody.
  RecompGameData::Initialize(runtime_config.game_root.string(),
                             runtime_config.user_directory.string());

  NetPlay::SetGameIdentity(inspected.metadata->disc_id,
                           CompatibilityFingerprint(runtime_config, *inspected.metadata));

  SessionUI ui(game);
  const NetPlay::NetTraversalConfig direct{};
  std::unique_ptr<NetPlay::NetPlayServer> server;
  std::unique_ptr<NetPlay::NetPlayClient> client;

  // Hosting can fail on a busy port, which is recoverable: offer to join the
  // host that already owns it, or retry. Headless keeps the old behaviour --
  // a script has nobody to ask and wants the exit code.
  while (options.role == NetplayRole::Host) {
    Log("hosting on port " + std::to_string(options.port));
    ui.SetHosting(true);
    server =
        std::make_unique<NetPlay::NetPlayServer>(options.port, false, &ui, direct);
    if (server->is_connected)
      break;

    Log("could not open port " + std::to_string(options.port) +
        " (already in use?)");
    server.reset();
    if (runtime_config.headless) {
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::Failed);
    }
    const PortErrorChoice choice =
        ShowPortError(runtime_config.window_system, options.port, options.address);
    if (choice == PortErrorChoice::Quit) {
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::Failed);
    }
    if (choice == PortErrorChoice::Join) {
      Log("joining the existing host instead");
      options.role = NetplayRole::Join;
      ui.SetHosting(false);
      break;
    }
    // Retry: the other instance may have shut down since.
  }

  if (options.role == NetplayRole::Host) {
    // Fixed-delay, not host input authority: both peers run the same inputs on
    // the same frame, which is the model the determinism work validated.
    server->SetHostInputAuthority(false);
    if (options.buffer != "auto") {
      try {
        server->AdjustPadBufferSize(
            static_cast<unsigned int>(std::stoul(options.buffer)));
      } catch (const std::exception &) {
        Log("ignoring unparsable --buffer '" + options.buffer + "'");
      }
    }
    server->ChangeGame(game->GetSyncIdentifier(), inspected.metadata->game_name);
    // The host plays through a local client too, so it shares one code path
    // with the joiners.
    client = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &ui, options.nickname, direct);
  } else {
    Log("connecting to " + options.address + ":" + std::to_string(options.port));
    client = std::make_unique<NetPlay::NetPlayClient>(
        options.address, options.port, &ui, options.nickname, direct);
  }

  if (!client->IsConnected()) {
    const std::string error = ui.Error();
    Log(error.empty() ? "could not connect to the host" : error);
    // A rejected handshake is not an unreachable host, and a script that
    // retries on HostUnavailable would retry this one forever. The reason text
    // is what the client's error path produced, so match on what it says about
    // the game rather than inventing a second channel for it.
    const bool mismatch = error.find("different game") != std::string::npos ||
                          error.find("differently built") != std::string::npos;
    // On screen, not just in the log: this is the moment the player is standing
    // there wondering why nothing happened.
    if (!runtime_config.headless) {
      ShowConnectError(runtime_config.window_system,
                       error.empty() ? "The host did not respond. Check the address, the port, "
                                       "and that the host has started the session."
                                     : error,
                       SessionLogPath());
    }
    client.reset();
    server.reset();
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(mismatch ? NetplayExitCode::CompatibilityMismatch
                                     : NetplayExitCode::HostUnavailable);
  }
  Log("connected as '" + options.nickname + "'");

  const std::chrono::seconds lobby_timeout(options.lobby_timeout);
  const std::string boot_path = game->GetFilePath();
  int result = 0;

  // Interactive lobby when there is a screen to draw it on. Headless keeps the
  // flag-driven auto-start below, which is what the scripted two-instance tests
  // use -- they must not start needing a human to click Start.
  if (!runtime_config.headless) {
    if (!RunLobbyWindow(runtime_config, options, ui, *client, server.get(),
                        boot_path)) {
      Log("lobby closed");
      client->Stop();
      client.reset();
      server.reset();
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return 0;
    }
    // RunLobbyWindow only returns true once StartGame has armed netplay and
    // the boot data has been handed to the runtime.
    if (!NetPlay::IsNetPlayRunning()) {
      Log("netplay did not arm; refusing to boot");
      client->Stop();
      client.reset();
      server.reset();
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::Failed);
    }
    LogPadRouting(*client);
    Log("netplay armed; booting");
    auto created = Runtime::Create(std::move(runtime_config));
    if (!created) {
      Log("initialization failed: " + created.error->message);
      result = 1;
    } else {
      ui.SetRuntime(created.runtime.get());
      const RuntimeRunResult run_result = created.runtime->Run();
      ui.SetRuntime(nullptr);
      if (run_result.error) {
        Log("run failed: " + run_result.error->message);
        result = 1;
      }
      created.runtime.reset();
    }
    if (ui.Desynced()) {
      Log("session ended DESYNCED at frame " + std::to_string(ui.DesyncFrame()));
      result = 1;
    } else if (result == 0) {
      Log("session ended cleanly, no desync reported");
    }
    client->Stop();
    client->StopGame();
    client.reset();
    server.reset();
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return result;
  }

  if (server) {
    const size_t expected = std::max<size_t>(options.players, 1);
    Log("waiting for " + std::to_string(expected) + " player(s)");
    if (!WaitFor(ui, *client, lobby_timeout,
                 [&] { return client->GetPlayers().size() >= expected; })) {
      Log("timed out waiting for players");
      result = static_cast<int>(NetplayExitCode::Failed);
    } else if (!WaitFor(ui, *client, std::chrono::seconds(30),
                        [&] { return client->DoAllPlayersHaveGame(); })) {
      Log("not every player has this game; refusing to start");
      result = static_cast<int>(NetplayExitCode::CompatibilityMismatch);
    } else {
      AssignPads(*server, client->GetPlayers());
      // The mapping is broadcast asynchronously; starting in the same breath
      // can race it, and a player whose pad has not landed yet is dropped from
      // the start.
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      Log("starting game");
      if (!server->RequestStartGame()) {
        Log("the host refused to start the game");
        result = static_cast<int>(NetplayExitCode::Failed);
      }
    }
  } else {
    Log("waiting for the host to start");
  }

  if (result == 0 && !WaitFor(ui, *client, lobby_timeout,
                              [&] { return ui.TakeStartRequest(); })) {
    Log("no start signal arrived");
    result = static_cast<int>(NetplayExitCode::Failed);
  }

  if (result == 0) {
    // OnMsgStartGame only *signals* the start. NetPlayClient::StartGame is what
    // calls NetPlay_Enable -- and until that happens NetPlay::IsNetPlayRunning()
    // is false, which means SI reads local pads instead of GetNetPads and
    // Dolphin's desync detection never arms. Booting straight from the signal
    // gives two independent single-player sessions that look like a clean
    // netplay run: it reports no desync precisely because nothing was checking.
    // That false pass is why the assertion below exists.
    if (!client->StartGame(game->GetFilePath())) {
      Log("the client refused to start the game");
      result = static_cast<int>(NetplayExitCode::Failed);
    }
  }

  if (result == 0) {
    // What this peer believes it owns. A client with zero local pads still
    // boots and runs perfectly happily -- it just never sends any input, and
    // the game sits in attract mode looking like it ignores the controller.
    const NetPlay::PadMappingArray &map = client->GetPadMapping();
    std::string map_text;
    for (size_t i = 0; i < map.size(); ++i)
      map_text += (i ? "," : "") + std::to_string(static_cast<int>(map[i]));
    Log("pad map [" + map_text + "], local pads = " +
        std::to_string(client->NumLocalPads()) +
        ", local player has a controller = " +
        (client->LocalPlayerHasControllerMapped() ? "yes" : "no"));
    if (client->NumLocalPads() == 0)
      Log("WARNING: no local pad -- this peer will send no input");
  }

  if (result == 0) {
    if (!NetPlay::IsNetPlayRunning()) {
      Log("netplay did not arm; refusing to boot (this would silently run as "
          "two unsynchronised single-player sessions)");
      result = static_cast<int>(NetplayExitCode::Failed);
    }
  }

  if (result == 0) {
    // StartGame produced this via BootGame; the runtime boots through it so the
    // session's synced settings and save data apply.
    if (auto boot_data = ui.TakeBootData()) {
      detail::SetBootSessionData(std::move(boot_data));
    } else {
      Log("no boot data arrived from netplay; refusing to boot");
      result = static_cast<int>(NetplayExitCode::Failed);
    }
  }

  if (result == 0) {
    LogPadRouting(*client);
    Log("netplay armed; booting");
    auto created = Runtime::Create(std::move(runtime_config));
    if (!created) {
      Log("initialization failed: " + created.error->message);
      result = 1;
    } else {
      ui.SetRuntime(created.runtime.get());
      const RuntimeRunResult run_result = created.runtime->Run();
      ui.SetRuntime(nullptr);
      if (run_result.error) {
        Log("run failed: " + run_result.error->message);
        result = 1;
      }
      created.runtime.reset();
    }
  }

  if (ui.Desynced()) {
    Log("session ended DESYNCED at frame " + std::to_string(ui.DesyncFrame()));
    result = 1;
  } else if (result == 0) {
    Log("session ended cleanly, no desync reported");
  }

  client->Stop();
  client->StopGame();
  client.reset();
  server.reset();
  detail::SetExternalUICommon(false);
  UICommon::Shutdown();
  return result;
}
} // namespace moderngekko::frontend
