// Netplay coverage against the PUBLIC RecompCore tree.
//
// This test used to be written against an unpushed RecompCore fork, so it had
// never compiled here at all -- and nothing noticed, because CI runs no tests.
// The symbols it needed and this tree does not have:
//
//   NetPlay::SetCompatibilityFingerprint, NetPlayClient::GetConnectionError,
//   ConnectionError::CompatibilityMismatch  -- a build-hash handshake that
//     rejects a mismatched peer at connect time.
//     LANDED 2026-08-25, in this tree rather than the fork, as
//     NetPlay::SetGameIdentity + ConnectionError::DifferentGame /
//     ::CompatibilityMismatch. Asserted at the end of this file. It exists
//     because three regions of the game now recompile, so two players can
//     hold discs that look identical -- same title, same internal name -- and
//     desync from the first frame.
//   NetPlayServer::CanStart, NetPlayServer::SetAdaptiveBuffer
//   NetPlayClient::SetLocalControllerCount / GetAssignedControllerCount /
//     GetPlayersSnapshot, and a 6th constructor argument for the local
//     controller count
//   MessageID::PadBufferRequest, NetPlay::INPUT_CHANNEL
//
// vendor/dolphin carries stock netplay, which has none of that. Those
// assertions are deleted rather than stubbed out -- a stub that always passes
// is worse than an absence. They are in git history if the fork ever lands.
//
// What survives is what this tree can genuinely assert: the ModernGekko-side
// compatibility fingerprint, which is pure computation, and a two-peer
// localhost lobby over the stock protocol. Exit codes are unchanged from the
// original for the checks that survived, so an old failure number still means
// the same thing.

// Needed again for `sf::Packet << MessageID`: the generic enum operator lives
// here, and the pad-routing case below is the only thing that serialises a
// message by hand. It was dropped when this test was ported off the fork's
// netplay API and no sf:: remained.
#include "Common/SFMLHelper.h"
#include "Core/Boot/Boot.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "UICommon/UICommon.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/runtime.hpp"
#include "netplay_compatibility.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
int DispatchA(CPUState *, std::uint32_t) { return 0; }
int DispatchB(CPUState *, std::uint32_t) { return 0; }

constexpr ModernGekkoRange module_ranges[] = {
    {0x80003100u, 0x80003120u},
};
constexpr std::uint64_t first_hashes[] = {0x123456789abcdef0u};
constexpr std::uint64_t second_hashes[] = {0x123456789abcdef0u};
constexpr std::uint64_t changed_hashes[] = {0x123456789abcdef1u};

const ModernGekkoModuleDesc first_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchA,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    first_hashes,
};

const ModernGekkoModuleDesc second_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchB,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    second_hashes,
};

const ModernGekkoModuleDesc changed_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchB,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    changed_hashes,
};
} // namespace

class TestUI final : public NetPlay::NetPlayUI {
public:
  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData>) override {}
  void StopGame() override {}
  bool IsHosting() const override { return false; }
  void Update() override {}
  void AppendChat(const std::string &) override {}
  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &) override {}
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}
  void OnMsgStartGame() override {}
  void OnMsgStopGame() override {}
  void OnMsgPowerButton() override {}
  void OnPlayerConnect(const std::string &) override {}
  void OnPlayerDisconnect(const std::string &) override {}
  void OnPadBufferChanged(u32 value) override { buffer = value; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32, const std::string &) override {}
  void OnConnectionLost() override {}
  void OnConnectionError(const std::string &message) override {
    error = message;
  }
  void OnTraversalError(Common::TraversalClient::FailureReason) override {}
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override {}
  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &,
               NetPlay::SyncIdentifierComparison *found) override {
    if (found)
      *found = NetPlay::SyncIdentifierComparison::DifferentGame;
    return {};
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
  void ShowChunkedProgressDialog(const std::string &, u64,
                                 std::span<const int>) override {}
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}

  std::string error;
  std::atomic<u32> buffer{0};
};

bool WaitFor(const auto &condition) {
  for (int i = 0; i < 100; ++i) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

int main() {
  moderngekko::GameMetadata metadata;
  metadata.disc_id = "TEST01";
  metadata.dol_sha256 = "dol";
  moderngekko::RuntimeConfig first_config;
  first_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&first_descriptor);
  moderngekko::RuntimeConfig second_config;
  second_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&second_descriptor);
  moderngekko::RuntimeConfig changed_config;
  changed_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&changed_descriptor);
  const std::string first_fingerprint =
      moderngekko::frontend::CompatibilityFingerprint(first_config, metadata);
  if (first_fingerprint !=
      moderngekko::frontend::CompatibilityFingerprint(second_config, metadata))
    return 11;
  if (first_fingerprint ==
      moderngekko::frontend::CompatibilityFingerprint(changed_config, metadata))
    return 12;

  // ---- identity handshake --------------------------------------------------
  //
  // The server captures its identity when constructed, so changing the
  // process-wide one afterwards gives the joining client a DIFFERENT identity
  // inside a single process -- which is the only way to test a two-sided
  // handshake without two machines.
  //
  // Ordering matters: this runs before UICommon::Init() and the shared lobby
  // below, so a rejected connection cannot leave a half-joined player in the
  // roster the later assertions count.
  {
    NetPlay::SetGameIdentity("GRSEAF", "fingerprint-A");
    TestUI reject_host_ui;
    auto reject_server = std::make_unique<NetPlay::NetPlayServer>(
        0, false, &reject_host_ui, NetPlay::NetTraversalConfig{});
    if (!reject_server->is_connected)
      return 20;

    // A different disc: what a Japanese or European copy looks like to a US
    // host. Rejected at connect, so IsConnected() is false and the dialog was
    // given a reason.
    NetPlay::SetGameIdentity("GRSJAF", "fingerprint-A");
    TestUI wrong_disc_ui;
    auto wrong_disc = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", reject_server->GetPort(), &wrong_disc_ui, "WrongDisc",
        NetPlay::NetTraversalConfig{});
    if (wrong_disc->IsConnected())
      return 21;
    if (wrong_disc_ui.error.find("different game") == std::string::npos)
      return 22;
    wrong_disc.reset();

    // Same disc, module built by different tools. The sync identifier cannot
    // see this one -- only the fingerprint can.
    NetPlay::SetGameIdentity("GRSEAF", "fingerprint-B");
    TestUI wrong_build_ui;
    auto wrong_build = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", reject_server->GetPort(), &wrong_build_ui, "WrongBuild",
        NetPlay::NetTraversalConfig{});
    if (wrong_build->IsConnected())
      return 23;
    if (wrong_build_ui.error.find("differently built") == std::string::npos)
      return 24;
    wrong_build.reset();

    // And the matching case still connects, or the check above would pass by
    // rejecting everything.
    NetPlay::SetGameIdentity("GRSEAF", "fingerprint-A");
    TestUI match_ui;
    auto match = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", reject_server->GetPort(), &match_ui, "Match",
        NetPlay::NetTraversalConfig{});
    if (!match->IsConnected())
      return 25;
    match.reset();
    reject_server.reset();
  }
  // Cleared, so the lobby below runs the way it always did -- an empty
  // fingerprint means "did not say" and skips the comparison entirely.
  NetPlay::SetGameIdentity("", "");

  const auto directory =
      std::filesystem::temp_directory_path() / "moderngekko-netplay-test";
  std::filesystem::remove_all(directory);
  UICommon::SetUserDirectory(directory.string());
  UICommon::Init();

  TestUI host_ui;
  TestUI first_ui;
  TestUI second_ui;
  TestUI invalid_ui;
  auto invalid = std::make_unique<NetPlay::NetPlayClient>(
      "invalid host", 2626, &invalid_ui, "Invalid",
      NetPlay::NetTraversalConfig{});
  if (invalid->IsConnected() || invalid_ui.error.empty())
    return 10;
  invalid.reset();

  // Port 0 asks the OS for a free one, so concurrent test runs cannot collide.
  auto server = std::make_unique<NetPlay::NetPlayServer>(
      0, false, &host_ui, NetPlay::NetTraversalConfig{});
  if (!server->is_connected)
    return 1;
  auto first = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &first_ui, "First",
      NetPlay::NetTraversalConfig{});
  if (!first->IsConnected())
    return 2;
  auto second = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Second",
      NetPlay::NetTraversalConfig{});
  if (!second->IsConnected())
    return 3;
  // The client constructor returns once its own connection is up, but the
  // roster is filled by the receive thread, so both peers have to be waited
  // for rather than read straight away.
  if (!WaitFor([&] { return first->GetPlayers().size() == 2; }) ||
      !WaitFor([&] { return second->GetPlayers().size() == 2; }))
    return 4;

  // Round-trips a real message: AdjustPadBufferSize broadcasts MessageID::
  // PadBuffer, which each client turns back into OnPadBufferChanged. It fails
  // if the lobby is connected but not actually exchanging packets.
  server->AdjustPadBufferSize(2);
  if (!WaitFor([&] { return first_ui.buffer == 2 && second_ui.buffer == 2; }))
    return 18;

  // ---- input routing -------------------------------------------------------
  //
  // The gap left open in #13. The server relays MessageID::PadData only from
  // the player who OWNS that port, and DISCONNECTS anyone sending for a port
  // they do not:
  //
  //     if (!IsValidPadIndex(m_pad_map, map) || m_pad_map.at(map) != player.pid)
  //         return 1;                       // NetPlayServer.cpp
  //
  // That guard is the whole reason a peer cannot inject input on someone else's
  // controller, and nothing exercised it. It is also what made this test
  // awkward to write: sending unmapped pad data does not fail politely, it
  // drops the connection.
  //
  // The RECEIVED pad data is deliberately not asserted here. GetNetPads()
  // returns false unless m_is_running is set, so observing the far side's
  // buffer needs a booted core -- an integration test, not this. What is
  // checked is the routing decision itself, which is the part that protects
  // the port.
  {
    NetPlay::PadMappingArray pads{};   // 0 = unassigned
    pads[0] = first->GetLocalPlayerId();
    server->SetPadMapping(pads);
    if (!WaitFor([&] { return first->GetPadMapping()[0] == first->GetLocalPlayerId(); }))
      return 19;

    const auto pad_packet = [](NetPlay::PadIndex map) {
      sf::Packet p;
      p << NetPlay::MessageID::PadData << map;
      p << static_cast<u16>(0x0100);                     // button
      p << u8(0) << u8(0) << u8(128) << u8(128)          // analogA/B, stickX/Y
        << u8(128) << u8(128) << u8(0) << u8(0) << u8(1); // substick, triggers, isConnected
      return p;
    };

    // The owner may send for its port, and must stay connected.
    first->SendAsync(pad_packet(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!first->IsConnected())
      return 20;

    // A peer that does NOT own port 0 must be dropped for claiming it. If this
    // ever stops disconnecting, one client can drive another's character.
    second->SendAsync(pad_packet(0));
    // Observed through the OTHER peer's roster: the server drops the offender
    // (OnData != 0 -> OnDisconnect), and that is what the rest of the lobby
    // sees. The offender's own IsConnected() is a less reliable witness.
    if (!WaitFor([&] { return first->GetPlayers().size() == 1; }))
      return 21;
    // ...and dropping the impostor must not take the legitimate peer with it.
    if (!first->IsConnected())
      return 22;
  }

  second.reset();
  first.reset();
  server.reset();
  UICommon::Shutdown();
  std::filesystem::remove_all(directory);
  return 0;
}
