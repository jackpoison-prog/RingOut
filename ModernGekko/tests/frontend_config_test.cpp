#include "frontend_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

int main() {
  namespace fs = std::filesystem;
  const fs::path directory =
      fs::temp_directory_path() /
      ("moderngekko-frontend-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string error;
  const std::string controller = "SDL/0/Test Controller";
  if (!moderngekko::frontend::SaveConfig(directory, "1920x1080", false,
                                         controller, &error))
    return 1;

  const auto loaded = moderngekko::frontend::LoadConfig(directory, false);
  if (!loaded || loaded.dolphin_scale != 3 || loaded.show_fps_in_title ||
      loaded.controller != controller) {
    return 2;
  }

  moderngekko::frontend::ConfigResult netplay_config = loaded;
  netplay_config.controllers = {controller, "SDL/1/Second Controller"};
  netplay_config.controller = controller;
  netplay_config.netplay_nickname = "Kirby";
  netplay_config.netplay_address = "192.168.1.50";
  netplay_config.netplay_port = 34567;
  netplay_config.netplay_buffer = "auto";
  if (!moderngekko::frontend::SaveConfig(directory, netplay_config, &error))
    return 6;
  const auto netplay_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!netplay_loaded ||
      netplay_loaded.controllers != netplay_config.controllers ||
      netplay_loaded.netplay_nickname != "Kirby" ||
      netplay_loaded.netplay_address != "192.168.1.50" ||
      netplay_loaded.netplay_port != 34567 ||
      netplay_loaded.netplay_buffer != "auto") {
    return 7;
  }

  auto invalid_netplay = netplay_config;
  invalid_netplay.netplay_address = "not a host";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 8;
  invalid_netplay = netplay_config;
  invalid_netplay.netplay_nickname = std::string(31, 'K');
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 9;
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, netplay_config.controllers, &error))
    return 3;
  if (moderngekko::frontend::ReadConfiguredController(directory) != controller)
    return 4;
  if (moderngekko::frontend::ReadConfiguredControllers(directory) !=
      netplay_config.controllers)
    return 10;

  std::ifstream input(directory / "Config" / "WiimoteNew.ini");
  const std::string generated{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
  if (!generated.contains("Buttons/A = `Shoulder L`\n") ||
      !generated.contains("Buttons/1 = `Button W`\n") ||
      !generated.contains("Buttons/2 = `Button S`\n") ||
      !generated.contains("Shake/X = `Trigger L`\n") ||
      !generated.contains("Extension = None\n") ||
      !generated.contains("Options/Sideways Wiimote = True\n") ||
      !generated.contains("[Wiimote2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("Nunchuk/")) {
    return 5;
  }

  const std::string custom =
      "[Wiimote1]\nDevice = SDL/9/Custom Controller\nButtons/1 = Custom\n";
  {
    std::ofstream output(directory / "Config" / "WiimoteNew.ini",
                         std::ios::trunc);
    output << custom;
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, netplay_config.controllers, &error,
          moderngekko::frontend::LocalMultiplayer::Disabled))
    return 11;
  std::ifstream custom_input(directory / "Config" / "WiimoteNew.ini");
  const std::string preserved{std::istreambuf_iterator<char>(custom_input),
                              std::istreambuf_iterator<char>()};
  if (preserved != custom || moderngekko::frontend::ReadConfiguredController(
                                 directory) != "SDL/9/Custom Controller")
    return 12;

  // A named pad drives the GC pad profile, not just the Wiimote one. This is
  // the Steam Deck case: before it, EnsureControllerConfig wrote a keyboard
  // profile unconditionally, so a machine with a pad and no keyboard had no
  // usable input at all.
  const fs::path pad_directory = directory / "pad";
  const std::string pad = "SDL/0/Test Gamepad";
  if (!moderngekko::frontend::EnsureControllerConfig(pad_directory, pad,
                                                     &error))
    return 13;
  std::ifstream pad_input(pad_directory / "Config" / "GCPadNew.ini");
  const std::string pad_config{std::istreambuf_iterator<char>(pad_input),
                               std::istreambuf_iterator<char>()};
  if (!pad_config.contains("Device = SDL/0/Test Gamepad\n") ||
      !pad_config.contains("Buttons/A = `Button S`\n") ||
      !pad_config.contains("Main Stick/Up = `Left Y+`\n") ||
      !pad_config.contains("C-Stick/Calibration = ") ||
      pad_config.contains("XInput2")) {
    return 14;
  }

  // No pad named and (in CI) none attached: the keyboard profile is still the
  // fallback, so a desktop with no hardware keeps booting into a playable game.
  if (moderngekko::frontend::DetectSdlGamepads().empty()) {
    const fs::path keyboard_directory = directory / "keyboard";
    if (!moderngekko::frontend::EnsureControllerConfig(
            keyboard_directory, std::span<const std::string>{}, &error))
      return 15;
    std::ifstream keyboard_input(keyboard_directory / "Config" /
                                 "GCPadNew.ini");
    const std::string keyboard_config{
        std::istreambuf_iterator<char>(keyboard_input),
        std::istreambuf_iterator<char>()};
    if (!keyboard_config.contains("XInput2") ||
        keyboard_config.contains("SDL/"))
      return 16;
  } else {
    // The same call on a machine that DOES have a pad, which is the path that
    // segfaulted on the Steam Deck: nothing named, so the span was repointed at
    // a vector of detected pads that then went out of scope, and the Wiimote
    // profile written afterwards read it back. CI has no pad and can never
    // reach this, so it only ever fires on real hardware -- which is precisely
    // where the bug lived.
    const fs::path detected_directory = directory / "detected";
    if (!moderngekko::frontend::EnsureControllerConfig(
            detected_directory, std::span<const std::string>{}, &error))
      return 17;
    std::ifstream detected_input(detected_directory / "Config" /
                                 "GCPadNew.ini");
    const std::string detected_config{
        std::istreambuf_iterator<char>(detected_input),
        std::istreambuf_iterator<char>()};
    if (!detected_config.contains("SDL/") ||
        !fs::is_regular_file(detected_directory / "Config" / "WiimoteNew.ini"))
      return 18;

    // Dolphin allocates device ids per (source, NAME), not per source: two pads
    // of DIFFERENT models are both id 0. Numbering them sequentially produced
    // "SDL/1/<name>" for a device Dolphin calls "SDL/0/<name>", which resolves
    // to nothing -- every binding reads unpressed and nothing reports an error.
    // Only a machine with two differently-named pads can catch it.
    const std::vector<std::string> pads =
        moderngekko::frontend::DetectSdlGamepads();
    for (std::size_t i = 0; i < pads.size(); ++i) {
      const std::size_t slash = pads[i].find('/', 4);
      if (slash == std::string::npos)
        return 19;
      const std::string name = pads[i].substr(slash);
      std::size_t expected = 0;
      for (std::size_t j = 0; j < i; ++j) {
        if (pads[j].ends_with(name))
          ++expected;
      }
      if (pads[i] != "SDL/" + std::to_string(expected) + name)
        return 20;
    }

    // A second physical pad has to arrive PLAYABLE. Port 2 is attached by
    // default now, and Dolphin's own defaults reach port 1 only, so without a
    // written [GCPad2] the game un-greys VS Battle for a player who cannot
    // move. The calibration line is the part no amount of hand-binding can
    // recover: the CONTROLS tab lists controls, and calibration is not one.
    if (pads.size() >= 2) {
      if (!detected_config.contains("[GCPad2]\n") ||
          !detected_config.contains("Device = " + pads[1] + "\n"))
        return 21;
      const std::size_t port2 = detected_config.find("[GCPad2]");
      const std::string second = detected_config.substr(port2);
      if (!second.contains("Main Stick/Calibration = ") ||
          !second.contains("C-Stick/Calibration = ") ||
          !second.contains("Triggers/L-Analog = ") ||
          !second.contains("Buttons/A = `Button S`\n"))
        return 22;
      // Both ports must get the SAME map: a player whose A button is a
      // different physical button from the other player's is a defect a test
      // can catch and a play session cannot.
      const std::size_t port1 = detected_config.find("[GCPad1]");
      const std::string first = detected_config.substr(port1, port2 - port1);
      // Everything after each section's Device line, so the header and the pad
      // name -- the only two things that are meant to differ -- drop out.
      const auto bindings = [](const std::string &section) {
        const std::size_t device = section.find("Device = ");
        return section.substr(section.find('\n', device) + 1);
      };
      if (bindings(first) != bindings(second))
        return 23;
    } else if (detected_config.contains("[GCPad2]")) {
      return 24;  // one pad must not claim port 2
    }

    // The upgrade path. An existing profile is never rewritten, but one written
    // before port 2 was attached has no [GCPad2] at all -- every install that
    // predates it, which is the one the bug was reported from. The section is
    // appended; nothing already in the file is touched; and a second run must
    // not append it twice.
    if (pads.size() >= 2) {
      const fs::path upgrade = directory / "upgrade";
      fs::create_directories(upgrade / "Config");
      const std::string port1_only =
          detected_config.substr(0, detected_config.find("[GCPad2]"));
      {
        std::ofstream output(upgrade / "Config" / "GCPadNew.ini",
                             std::ios::trunc);
        output << port1_only;
      }
      for (int pass = 0; pass < 2; ++pass) {
        if (!moderngekko::frontend::EnsureControllerConfig(
                upgrade, std::span<const std::string>{}, &error))
          return 25;
        std::ifstream upgraded_input(upgrade / "Config" / "GCPadNew.ini");
        const std::string upgraded{
            std::istreambuf_iterator<char>(upgraded_input),
            std::istreambuf_iterator<char>()};
        if (!upgraded.starts_with(port1_only))
          return 26;  // the player's own port 1 was altered
        const std::size_t first = upgraded.find("[GCPad2]");
        if (first == std::string::npos ||
            upgraded.find("[GCPad2]", first + 1) != std::string::npos)
          return 27;  // absent, or appended twice
        if (!upgraded.substr(first).contains("Main Stick/Calibration = "))
          return 28;
      }

      // The launch path passes the controller saved in config.ini, so the list
      // is non-empty on every install that has been through the frontend once.
      // Port 2 must still be mapped there: the old guard read a non-empty list
      // as "netplay" and silently disabled local two-player on exactly those
      // installs, which is how a Deck with two pads attached ended up with
      // Dolphin's own five-button stub for player 2. Only the caller can say
      // which it is, so netplay says so explicitly and everything else does not.
      const std::vector<std::string> saved = {pads[0]};
      const fs::path named = directory / "named-controller";
      fs::create_directories(named / "Config");
      {
        std::ofstream output(named / "Config" / "GCPadNew.ini", std::ios::trunc);
        output << port1_only;
      }
      if (!moderngekko::frontend::EnsureControllerConfig(named, saved, &error))
        return 29;
      std::ifstream named_input(named / "Config" / "GCPadNew.ini");
      const std::string named_config{std::istreambuf_iterator<char>(named_input),
                                     std::istreambuf_iterator<char>()};
      if (!named_config.contains("[GCPad2]\n") ||
          !named_config.contains("Device = " + pads[1] + "\n") ||
          !named_config.substr(named_config.find("[GCPad2]"))
               .contains("Main Stick/Calibration = "))
        return 30;

      // Port 2 must never be handed the device port 1 already holds. The
      // detected order is SDL's arrival order, not the port order: taking
      // detected[1] worked on a desktop where port 1 happened to be
      // detected[0], and on the Deck in Game Mode it gave both ports the same
      // pad -- two characters on one controller, which is unplayable for both
      // players rather than for one. Port 1 here names the pad SDL lists
      // SECOND, which is the ordering that broke it.
      const fs::path swapped = directory / "swapped-order";
      fs::create_directories(swapped / "Config");
      {
        std::string port1_other = port1_only;
        const std::size_t device = port1_other.find("Device = " + pads[0]);
        if (device == std::string::npos)
          return 33;
        port1_other.replace(device, std::string("Device = " + pads[0]).size(),
                            "Device = " + pads[1]);
        std::ofstream output(swapped / "Config" / "GCPadNew.ini",
                             std::ios::trunc);
        output << port1_other;
      }
      if (!moderngekko::frontend::EnsureControllerConfig(
              swapped, std::span<const std::string>{}, &error))
        return 34;
      std::ifstream swapped_input(swapped / "Config" / "GCPadNew.ini");
      const std::string swapped_config{
          std::istreambuf_iterator<char>(swapped_input),
          std::istreambuf_iterator<char>()};
      const std::size_t swapped_port2 = swapped_config.find("[GCPad2]");
      if (swapped_port2 == std::string::npos ||
          !swapped_config.substr(swapped_port2)
               .contains("Device = " + pads[0] + "\n"))
        return 35;

      // ...and netplay, whose list looks identical, must NOT get port 2: there
      // the entries are a per-machine assignment, not two pads on one desk.
      const fs::path netplay_dir = directory / "netplay-controllers";
      fs::create_directories(netplay_dir / "Config");
      {
        std::ofstream output(netplay_dir / "Config" / "GCPadNew.ini",
                             std::ios::trunc);
        output << port1_only;
      }
      if (!moderngekko::frontend::EnsureControllerConfig(
              netplay_dir, saved, &error,
              moderngekko::frontend::LocalMultiplayer::Disabled))
        return 31;
      std::ifstream netplay_input(netplay_dir / "Config" / "GCPadNew.ini");
      const std::string netplay_pads{
          std::istreambuf_iterator<char>(netplay_input),
          std::istreambuf_iterator<char>()};
      if (netplay_pads.contains("[GCPad2]"))
        return 32;
    }
  }

  fs::remove_all(directory);
  return 0;
}
