#pragma once

#include "frontend_config.hpp"
#include "moderngekko/runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace moderngekko::frontend {
enum class NetplayRole {
  Host,
  Join,
};

enum class NetplayExitCode {
  Failed = 1,
  InvalidConfiguration = 2,
  HostUnavailable = 10,
  VersionMismatch = 11,
  CompatibilityMismatch = 12,
  RoomFull = 13,
  GameRunning = 14,
  ServerFull = 15,
  NicknameRejected = 16,
  DifferentPlatform = 17,
};

struct NetplayOptions {
  NetplayRole role = NetplayRole::Join;
  std::string address = "127.0.0.1";
  std::uint16_t port = 2626;
  std::string nickname = "Player";
  std::string buffer = "auto";
  std::vector<std::string> controllers;
  // Host only: how many machines to wait for before starting. Upstream Dolphin
  // has no ready protocol, so this is what decides when the session begins.
  unsigned players = 2;
  // Seconds to wait for peers and for the start signal. A headless lobby must
  // not block forever; a hung run looks exactly like a slow one to a script.
  unsigned lobby_timeout = 120;
};

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options);
} // namespace moderngekko::frontend
