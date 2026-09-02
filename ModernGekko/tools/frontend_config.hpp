#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend {
struct ResolutionOption {
  const char *text;
  int dolphin_scale;
};

struct ConfigResult {
  int dolphin_scale = 0;
  std::string resolution;
  std::string controller;
  std::vector<std::string> controllers;
  bool show_fps_in_title = true;
  std::string netplay_nickname = "Player";
  std::string netplay_address = "127.0.0.1";
  std::uint16_t netplay_port = 2626;
  std::string netplay_buffer = "auto";
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

const std::vector<ResolutionOption> &SupportedResolutions();
ConfigResult LoadConfig(const std::filesystem::path &user_directory,
                        bool create_if_missing);
bool SaveConfig(const std::filesystem::path &user_directory,
                const ConfigResult &config, std::string *error);
bool SaveConfig(const std::filesystem::path &user_directory,
                std::string_view resolution, bool show_fps_in_title,
                std::string_view controller, std::string *error);
std::string
ReadConfiguredController(const std::filesystem::path &user_directory);
std::vector<std::string>
ReadConfiguredControllers(const std::filesystem::path &user_directory);
bool ControllerConfigExists(const std::filesystem::path &user_directory);
// True when the user directory already has a GameCube pad profile.
bool GCPadConfigExists(const std::filesystem::path &user_directory);
// Write a keyboard GCPadNew.ini. This is the default for a GameCube title: the
// existing generator only ever wrote WiimoteNew.ini, a leftover from this
// tree's Wii lineage, so a fresh user directory had no GC pad at all and the
// game was unplayable without hand-writing one. key_set selects between two
// disjoint layouts so two local instances can share one keyboard.
enum class KeyboardLayout { Player1, Player2 };
bool WriteKeyboardGCPadConfig(const std::filesystem::path &user_directory,
                              KeyboardLayout layout, std::string *message);
// Write a GCPadNew.ini bound to an SDL gamepad. `device` is a fully qualified
// Dolphin device name ("SDL/0/<pad name>"); DetectSdlGamepads produces them.
bool WriteGamepadGCPadConfig(const std::filesystem::path &user_directory,
                             std::string_view device, std::string *message);
// The same, one port per device: devices[0] drives port 1, devices[1] port 2.
// Port 2 needs its own section here because Dolphin gives default bindings to
// port 1 only, and the CONTROLS tab cannot reach a stick's calibration at all.
bool WriteGamepadGCPadConfig(const std::filesystem::path &user_directory,
                             std::span<const std::string> devices,
                             std::string *message);
// Connected SDL gamepads, as Dolphin device names, in Dolphin's own order --
// including its id numbering, which runs per (source, NAME) and not per source,
// so two pads of different models are BOTH "SDL/0/...". Getting that wrong
// yields a device that does not exist, and bindings written against it read as
// unpressed forever without anything reporting an error.
// Empty when there is no pad -- which is the signal to fall back to a keyboard
// profile. Enumerated rather than hardcoded: the Steam Deck's pad reaches us
// through Steam Input as a virtual X-Box 360 controller whose SDL name is NOT
// its evdev name, so any string written from memory is a guess.
std::vector<std::string> DetectSdlGamepads();
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::string_view controller,
                              std::string *message);
// Whether a second PRESENT gamepad may be mapped to port 2. Netplay passes
// Disabled: there the controller list is a per-machine netplay assignment, and
// giving the local player a second local pad would be wrong. Everything else
// wants Enabled -- note the ordinary launch path also passes a NON-EMPTY list,
// the controller saved in config.ini, so "the caller named a device" is NOT a
// usable proxy for "this is netplay".
enum class LocalMultiplayer { Enabled, Disabled };
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message,
                            LocalMultiplayer local = LocalMultiplayer::Enabled);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::string_view controller, std::string *message,
                            LocalMultiplayer local = LocalMultiplayer::Enabled);
} // namespace moderngekko::frontend
