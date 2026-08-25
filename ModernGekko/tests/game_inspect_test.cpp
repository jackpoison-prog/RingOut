#include "moderngekko/game.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

int main()
{
  const fs::path root = fs::temp_directory_path() / "moderngekko-game-inspect-test";
  fs::remove_all(root);
  fs::create_directories(root / "sys");
  fs::create_directories(root / "files");

  std::array<unsigned char, 0x60> boot{};
  const char id[] = "TEST01";
  std::copy_n(id, 6, boot.begin());
  boot[0x18] = 0x5d; boot[0x19] = 0x1c; boot[0x1a] = 0x9e; boot[0x1b] = 0xa3;
  const char name[] = "Synthetic Test Game";
  std::copy_n(name, sizeof(name), boot.begin() + 0x20);
  std::ofstream(root / "sys" / "boot.bin", std::ios::binary)
      .write(reinterpret_cast<const char*>(boot.data()), boot.size());

  std::array<unsigned char, 0x104> dol{};
  dol[0x02] = 0x01;  // Text section 0 file offset: 0x100.
  dol[0x48] = 0x80; dol[0x49] = 0x00; dol[0x4a] = 0x31; dol[0x4b] = 0x00;
  dol[0x93] = 0x04;
  dol[0xe0] = 0x80; dol[0xe1] = 0x00; dol[0xe2] = 0x31; dol[0xe3] = 0x00;
  std::ofstream(root / "sys" / "main.dol", std::ios::binary)
      .write(reinterpret_cast<const char*>(dol.data()), dol.size());

  const auto result = moderngekko::InspectGame(root);
  fs::remove_all(root);
  if (!result || result.metadata->disc_id != "TEST01" ||
      result.metadata->game_name != "Synthetic Test Game" ||
      result.metadata->platform != moderngekko::GamePlatform::Wii ||
      result.metadata->entry_point != 0x80003100u ||
      result.metadata->dol_sha256 !=
          "ee292f5fc3d0e5cfa32d951bd682a3cd2806c102e4a0a50300a2c480e21bcef6")
    return 1;
  // A DOL with no spin of that shape reports no idle loop, rather than zero.
  if (result.metadata->idle_pc)
    return 1;

  // The idle-loop scan. The core's idle-skip needs the OS spin loop's PC, and
  // that address moves with every build of a game, so InspectGame matches the
  // loop by shape -- which is what lets a disc from another region run at full
  // speed without a new constant in the runtime.
  const auto word = [](std::vector<unsigned char>& out, unsigned long w) {
    out.push_back(static_cast<unsigned char>(w >> 24));
    out.push_back(static_cast<unsigned char>(w >> 16));
    out.push_back(static_cast<unsigned char>(w >> 8));
    out.push_back(static_cast<unsigned char>(w));
  };
  const auto inspect_text = [&boot, &word](const std::vector<unsigned char>& text) {
    const fs::path r = fs::temp_directory_path() / "moderngekko-idle-pc-test";
    fs::remove_all(r);
    fs::create_directories(r / "sys");
    fs::create_directories(r / "files");
    std::ofstream(r / "sys" / "boot.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(boot.data()), boot.size());

    std::vector<unsigned char> image(0x100, 0);
    std::vector<unsigned char> field;
    word(field, 0x100);            // text[0] file offset
    std::copy(field.begin(), field.end(), image.begin() + 0x00);
    field.clear();
    word(field, 0x80003100);       // text[0] load address
    std::copy(field.begin(), field.end(), image.begin() + 0x48);
    field.clear();
    word(field, text.size());      // text[0] size
    std::copy(field.begin(), field.end(), image.begin() + 0x90);
    field.clear();
    word(field, 0x80003100);       // entry point
    std::copy(field.begin(), field.end(), image.begin() + 0xe0);
    image.insert(image.end(), text.begin(), text.end());
    std::ofstream(r / "sys" / "main.dol", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), image.size());

    auto inspected = moderngekko::InspectGame(r);
    fs::remove_all(r);
    return inspected;
  };

  std::vector<unsigned char> text;
  word(text, 0x80030000);  // lwz    r0, 0(r3)     -- a spin, but not through r13
  word(text, 0x28000000);  // cmplwi r0, 0
  word(text, 0x4182FFF8);  // beq    -8
  word(text, 0x800D8BF0);  // lwz    r0, -0x7410(r13)
  word(text, 0x2C000000);  // cmpwi  r0, 0         -- signed: the other r13 spin
  word(text, 0x4182FFF8);  // beq    -8               both real discs carry
  word(text, 0x800D8BF0);  // lwz    r0, -0x7410(r13)  <- the idle loop
  word(text, 0x28000000);  // cmplwi r0, 0
  word(text, 0x4182FFF8);  // beq    -8
  const auto one = inspect_text(text);
  if (!one || one.metadata->idle_pc != 0x80003118u)
    return 1;

  // Two candidates of the same shape are as unusable as none: picking the wrong
  // spin costs the same half of the frame rate, silently.
  word(text, 0x800D8BF0);
  word(text, 0x28000000);
  word(text, 0x4182FFF8);
  const auto ambiguous = inspect_text(text);
  if (!ambiguous || ambiguous.metadata->idle_pc)
    return 1;
  return 0;
}
