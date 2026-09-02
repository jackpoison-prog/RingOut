#include "frontend_config.hpp"

#include <algorithm>
#include <ranges>
#include <cctype>
#include <charconv>
#include <fstream>
#include <ostream>
#include <string_view>

#ifndef MODERNGEKKO_NO_SDL_GAMEPADS
#include <SDL3/SDL.h>
#endif

namespace fs = std::filesystem;

namespace moderngekko::frontend {
namespace {
std::string Trim(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ValidNetplayAddress(std::string_view value) {
  if (value.empty() || value.size() > 253)
    return false;
  return std::ranges::all_of(value, [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_';
  });
}

} // namespace

const std::vector<ResolutionOption> &SupportedResolutions() {
  // These are the output-resolution labels used by Dolphin's integer EFB
  // scales.
  static const std::vector<ResolutionOption> resolutions = {
      {"640x528", 1},   {"1280x720", 2},  {"1920x1080", 3},  {"2560x1440", 4},
      {"3840x2160", 6}, {"5120x2880", 8}, {"7680x4320", 12},
  };
  return resolutions;
}

ConfigResult LoadConfig(const fs::path &user_directory,
                        bool create_if_missing) {
  const fs::path path = user_directory / "config.ini";
  if (!fs::exists(path) && create_if_missing) {
    std::string error;
    if (!SaveConfig(user_directory, "1920x1080", true, {}, &error))
      return {.error = std::move(error)};
  }

  std::ifstream file(path);
  if (!file)
    return {.error = "can't open " + path.string()};

  ConfigResult config;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' ||
        trimmed[0] == '[')
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos)
      return {.error = "invalid config.ini line: " + trimmed};
    const std::string key = Lower(Trim(trimmed.substr(0, separator)));
    const std::string raw_value = Trim(trimmed.substr(separator + 1));
    const std::string value = Lower(raw_value);
    if (key == "resolution")
      config.resolution = value;
    else if (key == "controller")
      config.controller = raw_value;
    else if (key.starts_with("controller") && key.size() == 11 &&
             key.back() >= '1' && key.back() <= '4') {
      const std::size_t index = static_cast<std::size_t>(key.back() - '1');
      if (config.controllers.size() <= index)
        config.controllers.resize(index + 1);
      config.controllers[index] = raw_value;
    } else if (key == "show_fps_in_title") {
      if (value == "true" || value == "1" || value == "yes" || value == "on")
        config.show_fps_in_title = true;
      else if (value == "false" || value == "0" || value == "no" ||
               value == "off")
        config.show_fps_in_title = false;
      else
        return {.error = "show_fps_in_title must be true or false"};
    } else if (key == "nickname")
      config.netplay_nickname = raw_value;
    else if (key == "address")
      config.netplay_address = raw_value;
    else if (key == "port") {
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          raw_value.data(), raw_value.data() + raw_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != raw_value.data() + raw_value.size() || port == 0 ||
          port > 65535)
        return {.error = "netplay port must be between 1 and 65535"};
      config.netplay_port = static_cast<std::uint16_t>(port);
    } else if (key == "buffer") {
      if (value != "auto") {
        unsigned int frames = 0;
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), frames);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || frames < 1 ||
            frames > 20)
          return {.error =
                      "netplay buffer must be auto or a value from 1 to 20"};
      }
      config.netplay_buffer = value;
    }
  }
  if (config.resolution.empty())
    return {.error = "config.ini is missing resolution=<width>x<height>"};

  std::erase(config.controllers, std::string{});
  if (config.controllers.empty() && !config.controller.empty())
    config.controllers.push_back(config.controller);
  if (config.controller.empty() && !config.controllers.empty())
    config.controller = config.controllers.front();
  if (config.netplay_nickname.empty())
    return {.error = "netplay nickname cannot be empty"};
  if (config.netplay_nickname.size() > 30)
    return {.error = "netplay nickname cannot exceed 30 characters"};
  if (!ValidNetplayAddress(config.netplay_address))
    return {.error = "netplay address must be an IPv4 address or hostname"};

  for (const ResolutionOption &option : SupportedResolutions()) {
    if (config.resolution == option.text) {
      config.dolphin_scale = option.dolphin_scale;
      return config;
    }
  }

  // Dolphin also accepts exact raw EFB multiples even when they do not have a
  // common display label.
  for (int scale = 1; scale <= 12; ++scale) {
    const std::string raw =
        std::to_string(640 * scale) + "x" + std::to_string(528 * scale);
    if (config.resolution == raw) {
      config.dolphin_scale = scale;
      return config;
    }
  }

  return {.error = "unsupported Dolphin internal resolution '" +
                   config.resolution +
                   "'; use a listed display resolution or an exact 640x528 "
                   "multiple up to 12x"};
}

bool SaveConfig(const fs::path &user_directory, const ConfigResult &config,
                std::string *error) {
  if (config.resolution.empty() || config.netplay_nickname.empty() ||
      config.netplay_nickname.size() > 30 ||
      config.netplay_nickname.find_first_of("\r\n") != std::string::npos ||
      !ValidNetplayAddress(config.netplay_address) ||
      config.netplay_address.find_first_of("\r\n") != std::string::npos ||
      config.netplay_port == 0) {
    if (error)
      *error = "invalid frontend settings";
    return false;
  }
  if (config.netplay_buffer != "auto") {
    unsigned int frames = 0;
    const auto parsed = std::from_chars(
        config.netplay_buffer.data(),
        config.netplay_buffer.data() + config.netplay_buffer.size(), frames);
    if (parsed.ec != std::errc{} ||
        parsed.ptr !=
            config.netplay_buffer.data() + config.netplay_buffer.size() ||
        frames < 1 || frames > 20) {
      if (error)
        *error = "netplay buffer must be auto or a value from 1 to 20";
      return false;
    }
  }
  std::error_code ec;
  fs::create_directories(user_directory, ec);
  if (ec) {
    if (error)
      *error = "can't create user directory: " + ec.message();
    return false;
  }
  std::ofstream file(user_directory / "config.ini", std::ios::trunc);
  if (!file) {
    if (error)
      *error = "can't write " + (user_directory / "config.ini").string();
    return false;
  }
  file << "# ModernGekko frontend settings\n"
          "# This is Dolphin's internal render target, not the window size.\n"
          "[Video]\n"
          "resolution="
       << config.resolution << '\n'
       << "show_fps_in_title=" << (config.show_fps_in_title ? "true" : "false")
       << '\n'
       << "[Input]\n";
  for (std::size_t i = 0; i < config.controllers.size() && i < 4; ++i) {
    if (config.controllers[i].find_first_of("\r\n") != std::string::npos) {
      if (error)
        *error = "controller device cannot contain a newline";
      return false;
    }
    file << "controller" << i + 1 << '=' << config.controllers[i] << '\n';
  }
  file << "[Netplay]\n"
       << "nickname=" << config.netplay_nickname << '\n'
       << "address=" << config.netplay_address << '\n'
       << "port=" << config.netplay_port << '\n'
       << "buffer=" << config.netplay_buffer << '\n';
  return true;
}

bool SaveConfig(const fs::path &user_directory, std::string_view resolution,
                bool show_fps_in_title, std::string_view controller,
                std::string *error) {
  ConfigResult config = LoadConfig(user_directory, false);
  if (!config)
    config = {};
  config.resolution = resolution;
  config.show_fps_in_title = show_fps_in_title;
  config.controller = controller;
  config.controllers.clear();
  if (!controller.empty())
    config.controllers.emplace_back(controller);
  return SaveConfig(user_directory, config, error);
}

std::string ReadConfiguredController(const fs::path &user_directory) {
  const std::vector<std::string> controllers =
      ReadConfiguredControllers(user_directory);
  return controllers.empty() ? std::string{} : controllers.front();
}

std::vector<std::string>
ReadConfiguredControllers(const fs::path &user_directory) {
  std::ifstream input(user_directory / "Config" / "WiimoteNew.ini");
  std::vector<std::string> controllers;
  std::string line;
  std::size_t wiimote = 4;
  while (std::getline(input, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
      wiimote = 4;
      if (trimmed.size() == 10 && trimmed.starts_with("[Wiimote") &&
          trimmed[8] >= '1' && trimmed[8] <= '4')
        wiimote = static_cast<std::size_t>(trimmed[8] - '1');
      continue;
    }
    if (wiimote >= 4)
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator != std::string::npos &&
        Trim(trimmed.substr(0, separator)) == "Device") {
      const std::string device = Trim(trimmed.substr(separator + 1));
      if (!device.empty()) {
        if (controllers.size() <= wiimote)
          controllers.resize(wiimote + 1);
        controllers[wiimote] = device;
      }
    }
  }
  std::erase(controllers, std::string{});
  return controllers;
}

bool ControllerConfigExists(const fs::path &user_directory) {
  std::error_code ec;
  return fs::is_regular_file(user_directory / "Config" / "WiimoteNew.ini", ec);
}

bool GCPadConfigExists(const fs::path &user_directory) {
  std::error_code ec;
  return fs::is_regular_file(user_directory / "Config" / "GCPadNew.ini", ec);
}

bool WriteKeyboardGCPadConfig(const fs::path &user_directory,
                              KeyboardLayout layout, std::string *message) {
  // Dolphin's Linux keyboard/mouse device is the X master pointer/keyboard
  // pair, exposed by the XInput2 backend under the pointer's name. The game
  // runs under XWayland here, so this works on a Wayland session too.
  constexpr const char *kDevice = "XInput2/0/Virtual core pointer";

  // Two disjoint layouts, so one keyboard can drive two local instances --
  // which is exactly what a two-peer netplay session on one machine needs, and
  // is unavoidable there: input has to keep working without window focus, so
  // both instances see every key.
  struct Keys {
    const char *up, *down, *left, *right;
    const char *a, *b, *x, *y, *z, *l, *r, *start;
  };
  const Keys keys = (layout == KeyboardLayout::Player1)
                        ? Keys{"Up", "Down", "Left", "Right", "Z", "X",
                               "C", "V", "F", "A", "S", "Return"}
                        : Keys{"I", "K", "J", "L", "B", "N",
                               "M", "comma", "H", "G", "T", "Y"};

  const fs::path destination = user_directory / "Config" / "GCPadNew.ini";
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }

  // Only port 1 is mapped. Under netplay each machine supplies one pad and the
  // host's mapping decides which in-game port it drives, so a local port 2
  // would be wrong there; for single player the game only needs port 1.
  output << "[GCPad1]\n"
         << "Device = " << kDevice << '\n'
         << "Buttons/A = `" << keys.a << "`\n"
         << "Buttons/B = `" << keys.b << "`\n"
         << "Buttons/X = `" << keys.x << "`\n"
         << "Buttons/Y = `" << keys.y << "`\n"
         << "Buttons/Z = `" << keys.z << "`\n"
         << "Buttons/Start = `" << keys.start << "`\n"
         << "Triggers/L = `" << keys.l << "`\n"
         << "Triggers/R = `" << keys.r << "`\n"
         << "D-Pad/Up = `" << keys.up << "`\n"
         << "D-Pad/Down = `" << keys.down << "`\n"
         << "D-Pad/Left = `" << keys.left << "`\n"
         << "D-Pad/Right = `" << keys.right << "`\n"
         // The stick as well as the D-pad: this game reads movement from the
         // analog stick, and a D-pad-only binding leaves the character rooted.
         << "Main Stick/Up = `" << keys.up << "`\n"
         << "Main Stick/Down = `" << keys.down << "`\n"
         << "Main Stick/Left = `" << keys.left << "`\n"
         << "Main Stick/Right = `" << keys.right << "`\n";
  output.close();
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  if (message)
    *message = std::string("keyboard mapped (") +
               (layout == KeyboardLayout::Player1 ? "arrows + ZXCV"
                                                 : "IJKL + BNM") +
               ")";
  return true;
}

std::vector<std::string> DetectSdlGamepads() {
  std::vector<std::string> devices;
#ifndef MODERNGEKKO_NO_SDL_GAMEPADS

  // Refcounted, and this runs before the emulator brings up its own input, so
  // initialising here does not disturb Dolphin's later SDL use. Quit only what
  // we started. No video subsystem: gamepads enumerate headless, which is what
  // Game Mode and CI both need.
  const bool started = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!started)
    return devices;

  int count = 0;
  SDL_JoystickID *const ids = SDL_GetGamepads(&count);
  if (ids != nullptr) {
    for (int i = 0; i < count; ++i) {
      // Dolphin names a device "<source>/<id>/<name>", numbering ids per source
      // in the order it adds them, and its SDL backend takes the name from
      // SDL_GetGamepadName -- so enumerating in SDL's order reproduces it.
      SDL_Gamepad *const pad = SDL_OpenGamepad(ids[i]);
      if (pad == nullptr)
        continue;
      const char *const name = SDL_GetGamepadName(pad);
      if (name != nullptr && *name != '\0') {
        // Dolphin numbers ids per (SOURCE, NAME) pair, NOT per source:
        // ControllerInterface::AddDevice's is_id_in_use compares source, name
        // and id together. So two pads of DIFFERENT models are both id 0, and
        // only two of the SAME model are 0 and 1. Counting sequentially named a
        // second, differently-modelled pad "SDL/1/<name>" where Dolphin calls
        // it "SDL/0/<name>" -- a device that does not exist. Dolphin resolves a
        // binding against an absent device to nothing at all, so the pad would
        // read centred and unpressed and NOTHING would report an error.
        const std::string suffix = std::string("/") + name;
        std::size_t id = 0;
        for (const std::string &seen : devices) {
          if (seen.ends_with(suffix))
            ++id;
        }
        devices.push_back("SDL/" + std::to_string(id) + suffix);
      }
      SDL_CloseGamepad(pad);
    }
    SDL_free(ids);
  }

  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
  return devices;
}

namespace {
// The default map, emitted for whichever port is being bound.
//
// Port 1 and port 2 must get the SAME map. Dolphin's own defaults reach port 1
// only -- InputConfig::LoadConfig clears the rest so four pads cannot all land
// on one device -- so whatever port 2 does not get here, a second player has to
// bind by hand in the CONTROLS tab. Two of these lines cannot be bound by hand
// at ALL: that tab lists controls, and Calibration is a group setting, not a
// control. A hand-bound port 2 is therefore stuck with a square stick gate, and
// in practice a player stops at the buttons and leaves the analog triggers
// unbound too.
//
// Input names are Dolphin's SDL gamepad names (s_sdl_button_names /
// s_sdl_axis_names in SDLGamepad.h), not SDL's own constants. The vertical
// axes read `Left Y+` for up because that backend already inverts them to
// match XInput -- do not "fix" the sign here.
//
// Face buttons follow the GameCube's physical layout rather than the labels:
// A is the big south button, B east. Z sits on the right shoulder because the
// GameCube has one Z; L/R are the analog triggers, and are mapped as both
// digital and analog so a full press registers as a click.
void WriteGCPadSection(std::ostream &output, int port,
                       std::string_view device) {
  output << "[GCPad" << port << "]\n"
         << "Device = " << device << '\n'
         << "Buttons/A = `Button S`\n"
            "Buttons/B = `Button E`\n"
            "Buttons/X = `Button W`\n"
            "Buttons/Y = `Button N`\n"
            "Buttons/Z = `Shoulder R`\n"
            "Buttons/Start = `Start`\n"
            "Triggers/L = `Trigger L`\n"
            "Triggers/R = `Trigger R`\n"
            "Triggers/L-Analog = `Trigger L`\n"
            "Triggers/R-Analog = `Trigger R`\n"
            "D-Pad/Up = `Pad N`\n"
            "D-Pad/Down = `Pad S`\n"
            "D-Pad/Left = `Pad W`\n"
            "D-Pad/Right = `Pad E`\n"
            "Main Stick/Up = `Left Y+`\n"
            "Main Stick/Down = `Left Y-`\n"
            "Main Stick/Left = `Left X-`\n"
            "Main Stick/Right = `Left X+`\n"
            // Without a calibration line Dolphin assumes a square gate and the
            // diagonals never reach full deflection.
            "Main Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 "
            "141.42 100.00 141.42\n"
            "C-Stick/Up = `Right Y+`\n"
            "C-Stick/Down = `Right Y-`\n"
            "C-Stick/Left = `Right X-`\n"
            "C-Stick/Right = `Right X+`\n"
            "C-Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 "
            "100.00 141.42\n";
}

// True when GCPadNew.ini already has a [GCPad2] section, matched on its own
// line so a device name that happens to contain the text cannot fake it.
bool HasGCPad2Section(const fs::path &user_directory) {
  std::ifstream input(user_directory / "Config" / "GCPadNew.ini");
  std::string line;
  while (std::getline(input, line)) {
    if (Trim(line) == "[GCPad2]")
      return true;
  }
  return false;
}
// The device port 1 is bound to, or empty if there is no [GCPad1] Device line.
// Needed because the detected order is NOT the port order: SDL enumerates in
// whatever order the devices arrived, so detected[1] is simply "the second pad
// SDL listed", which on the Steam Deck in Game Mode was the pad port 1 already
// held. Mapping port 2 onto port 1's device gives both players one pad, which
// is worse than leaving port 2 alone -- neither of them can then play.
std::string ReadGCPad1Device(const fs::path &user_directory) {
  std::ifstream input(user_directory / "Config" / "GCPadNew.ini");
  std::string line;
  bool in_port1 = false;
  while (std::getline(input, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.starts_with("[") && trimmed.ends_with("]")) {
      in_port1 = trimmed == "[GCPad1]";
      continue;
    }
    if (!in_port1 || !trimmed.starts_with("Device"))
      continue;
    const std::size_t equals = trimmed.find('=');
    if (equals != std::string::npos)
      return Trim(trimmed.substr(equals + 1));
  }
  return {};
}
} // namespace

// Writes one section per device: devices[0] drives port 1, devices[1] port 2.
bool WriteGamepadGCPadConfig(const fs::path &user_directory,
                             std::span<const std::string> devices,
                             std::string *message) {
  if (devices.empty()) {
    if (message)
      *message = "no gamepad to map";
    return false;
  }
  for (const std::string &device : devices) {
    if (device.empty() || device.find_first_of("\r\n") != std::string::npos) {
      if (message)
        *message = "invalid gamepad device name";
      return false;
    }
  }

  const fs::path destination = user_directory / "Config" / "GCPadNew.ini";
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }

  for (std::size_t i = 0; i < devices.size(); ++i)
    WriteGCPadSection(output, static_cast<int>(i) + 1, devices[i]);

  output.close();
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  if (message) {
    *message = "gamepad mapped (" + devices[0] + ")";
    for (std::size_t i = 1; i < devices.size(); ++i)
      *message += ", port " + std::to_string(i + 1) + " (" + devices[i] + ")";
  }
  return true;
}

bool WriteGamepadGCPadConfig(const fs::path &user_directory,
                             std::string_view device, std::string *message) {
  const std::string value(device);
  return WriteGamepadGCPadConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message) {
  if (controllers.empty() || controllers.size() > 4) {
    if (message)
      *message = "select between one and four connected SDL gamepads";
    return false;
  }
  for (const std::string &controller : controllers) {
    if (controller.empty() ||
        controller.find_first_of("\r\n") != std::string_view::npos) {
      if (message)
        *message = "select connected SDL gamepads";
      return false;
    }
  }

  const fs::path destination = user_directory / "Config" / "WiimoteNew.ini";
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    output << "[Wiimote" << i + 1 << "]\n";
    if (i >= controllers.size())
      continue;
    output << "Device = " << controllers[i] << '\n'
           << "Buttons/A = `Shoulder L`\n"
              "Buttons/B = `Shoulder R`\n"
              "Buttons/1 = `Button W`\n"
              "Buttons/2 = `Button S`\n"
              "Buttons/- = Back\n"
              "Buttons/+ = Start\n"
              "Buttons/Home = Guide\n"
              "D-Pad/Up = `Pad N`\n"
              "D-Pad/Down = `Pad S`\n"
              "D-Pad/Left = `Pad W`\n"
              "D-Pad/Right = `Pad E`\n"
              "IR/Up = `Cursor Y-`\n"
              "IR/Down = `Cursor Y+`\n"
              "IR/Left = `Cursor X-`\n"
              "IR/Right = `Cursor X+`\n"
              "Shake/X = `Trigger L`\n"
              "Shake/Y = `Trigger R`\n"
              "Shake/Z = `Trigger L`\n"
              "IRPassthrough/Object 1 X = `IR Object 1 X`\n"
              "IRPassthrough/Object 1 Y = `IR Object 1 Y`\n"
              "IRPassthrough/Object 1 Size = `IR Object 1 Size`\n"
              "IRPassthrough/Object 2 X = `IR Object 2 X`\n"
              "IRPassthrough/Object 2 Y = `IR Object 2 Y`\n"
              "IRPassthrough/Object 2 Size = `IR Object 2 Size`\n"
              "IRPassthrough/Object 3 X = `IR Object 3 X`\n"
              "IRPassthrough/Object 3 Y = `IR Object 3 Y`\n"
              "IRPassthrough/Object 3 Size = `IR Object 3 Size`\n"
              "IRPassthrough/Object 4 X = `IR Object 4 X`\n"
              "IRPassthrough/Object 4 Y = `IR Object 4 Y`\n"
              "IRPassthrough/Object 4 Size = `IR Object 4 Size`\n"
              "IMUAccelerometer/Up = `Accel Up`\n"
              "IMUAccelerometer/Down = `Accel Down`\n"
              "IMUAccelerometer/Left = `Accel Left`\n"
              "IMUAccelerometer/Right = `Accel Right`\n"
              "IMUAccelerometer/Forward = `Accel Forward`\n"
              "IMUAccelerometer/Backward = `Accel Backward`\n"
              "IMUGyroscope/Pitch Up = `Gyro Pitch Up`\n"
              "IMUGyroscope/Pitch Down = `Gyro Pitch Down`\n"
              "IMUGyroscope/Roll Left = `Gyro Roll Left`\n"
              "IMUGyroscope/Roll Right = `Gyro Roll Right`\n"
              "IMUGyroscope/Yaw Left = `Gyro Yaw Left`\n"
              "IMUGyroscope/Yaw Right = `Gyro Yaw Right`\n"
              "Rumble/Motor = Motor\n"
              "Extension = None\n"
              "Options/Sideways Wiimote = True\n";
  }
  output << "[BalanceBoard]\n";
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  if (message)
    *message = std::to_string(controllers.size()) + " sideways Wii Remote" +
               (controllers.size() == 1 ? " mapped" : "s mapped");
  return true;
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::string_view controller,
                              std::string *message) {
  const std::string value(controller);
  return GenerateControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message, LocalMultiplayer local) {
  // A GameCube title needs GCPadNew.ini, and nothing here ever wrote one. A
  // fresh user directory gets a pad if one is connected and a keyboard
  // otherwise; an existing profile is never touched.
  //
  // The keyboard used to be unconditional, which is why the Steam Deck detected
  // no input at all in Game Mode: it has a built-in controller and no keyboard,
  // and the CONTROLS tab that could have fixed it is behind a menu you need
  // working input to reach.
  // Owns whatever DetectSdlGamepads() finds. It has to outlive the block
  // below, because `controllers` is a span and is repointed at it there: when
  // this vector lived inside that block, the span dangled the moment the block
  // ended and the GenerateControllerConfig call at the bottom read freed
  // memory. That crashed at startup on any machine with a pad connected and a
  // user directory fresh enough to have no WiimoteNew.ini -- the Steam Deck
  // every time, a desktop never, because with no pad detected the span was
  // left pointing at the caller's own storage.
  std::vector<std::string> detected;
  if (!GCPadConfigExists(user_directory)) {
    // An explicitly selected pad wins; otherwise ask the hardware.
    bool from_hardware = false;
    if (controllers.empty()) {
      detected = DetectSdlGamepads();
      controllers = detected;
      from_hardware = true;
    }

    // A second PHYSICALLY PRESENT pad gets port 2, which now defaults to an
    // attached controller. Without this the port is attached but blank, so the
    // game un-greys VS Battle for a player who then has to hand-bind twenty
    // controls before they can move -- and who cannot reach the stick
    // calibration at all, because it is not a bindable control. Only from
    // DETECTED hardware: an explicit list is a netplay controller assignment
    // and must not be reinterpreted as local two-player. Capped at two, the
    // ports that are actually attached and all the CONTROLS tab can configure.
    const std::size_t ports = from_hardware && local == LocalMultiplayer::Enabled
                                  ? std::min<std::size_t>(controllers.size(), 2)
                                  : 1;

    std::string pad_message;
    const bool wrote_pad =
        !controllers.empty() &&
        WriteGamepadGCPadConfig(user_directory, controllers.first(ports),
                                &pad_message);
    if (!wrote_pad)
      WriteKeyboardGCPadConfig(user_directory, KeyboardLayout::Player1,
                               &pad_message);
    if (message)
      *message = pad_message;
  }
  // An existing profile is never rewritten -- but one written before port 2 was
  // attached has no [GCPad2] at all, which is EVERY install predating it,
  // including the one the bug was reported from. Appending the section when it
  // is absent and a second pad is present supplies what was missing without
  // touching a line the player set.
  //
  // Gated on the CALLER saying this is not netplay, NOT on the controller list
  // being empty. That test was wrong: the ordinary launch path passes the
  // controller saved in config.ini, so on any install that has been through the
  // frontend once the list is non-empty and the whole feature silently did
  // nothing. Measured on the Deck 2026-09-01, two pads attached, same binary,
  // one line of config.ini apart: with `controller1=Keyboard` no [GCPad2] was
  // written and Dolphin saved its own five-button stub -- no sticks, no
  // triggers, no calibration -- and without it, the full map.
  if (GCPadConfigExists(user_directory) && !HasGCPad2Section(user_directory) &&
      local == LocalMultiplayer::Enabled) {
    if (detected.empty())
      detected = DetectSdlGamepads();
    // Not detected[1]: that is "the second pad SDL listed", which is only port
    // 2's pad when the enumeration order happens to match the profile. It did
    // on the desktop and did NOT on the Deck in Game Mode, where port 2 was
    // handed port 1's own device. Pick the first detected pad port 1 is not
    // already using instead, and map nothing when there is no such pad.
    const std::string port1 = ReadGCPad1Device(user_directory);
    const auto second = std::ranges::find_if(
        detected, [&port1](const std::string &d) { return d != port1; });
    if (detected.size() >= 2 && second != detected.end()) {
      std::ofstream append(user_directory / "Config" / "GCPadNew.ini",
                           std::ios::app);
      if (append) {
        WriteGCPadSection(append, 2, *second);
        append.close();
        if (append && message)
          *message = "port 2 mapped (" + *second + ")";
      }
    }
  }

  if (ControllerConfigExists(user_directory)) {
    if (message && message->empty())
      *message = "using existing controller profile";
    return true;
  }
  // The Wiimote profile is optional for a GameCube game: failing to map an SDL
  // gamepad must not stop a keyboard player from booting.
  std::string ignored;
  GenerateControllerConfig(user_directory, controllers, &ignored);
  return true;
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::string_view controller, std::string *message,
                            LocalMultiplayer local) {
  const std::string value(controller);
  return EnsureControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message, local);
}
} // namespace moderngekko::frontend
