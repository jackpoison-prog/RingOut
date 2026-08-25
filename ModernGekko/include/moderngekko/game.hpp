#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace moderngekko
{
enum class GamePlatform
{
  GameCube,
  Wii,
};

struct GameMetadata
{
  std::filesystem::path root;
  std::filesystem::path main_dol;
  std::string game_name;
  std::string disc_id;
  GamePlatform platform = GamePlatform::GameCube;
  std::uint32_t entry_point = 0;
  std::string dol_sha256;
  // Guest PC of the OS idle spin loop, when the DOL contains exactly one loop
  // of that shape. Empty when it does not, so the caller can tell "no idle
  // loop found" from "the loop is at address zero".
  std::optional<std::uint32_t> idle_pc;
};

struct GameInspectResult
{
  std::optional<GameMetadata> metadata;
  std::string error;

  explicit operator bool() const { return metadata.has_value(); }
};

GameInspectResult InspectGame(const std::filesystem::path& root);
}  // namespace moderngekko

namespace ModernGekko = moderngekko;
