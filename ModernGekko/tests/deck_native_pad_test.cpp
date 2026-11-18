// The Deck's own controls, read directly (src/runtime/deck_native_pad.hpp).
#include "deck_native_pad.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}
}  // namespace

int main() {
  using namespace RingOutDeckPad;
  Check(IsDeckSdlPad("SDL/0/Steam Virtual Gamepad"), "Steam's virtual pad is the Deck's");
  Check(IsDeckSdlPad("SDL/0/Steam Deck Controller"), "the Deck pad outside Steam");
  Check(!IsDeckSdlPad("SDL/1/Xbox One S Controller"), "an external pad is not");
  Check(!IsDeckSdlPad("XInput2/0/Virtual core pointer"), "the keyboard is not");
  Check(IsDeckNative("SteamDeck/0/Steam Deck") && !IsDeckNative("SDL/0/Steam Deck Controller"),
        "the native backend");

  // Every line of the default profile (tools/frontend_config.cpp).
  Check(Translate("`Button S`") == "`A`" && Translate("`Button E`") == "`B`" &&
            Translate("`Button W`") == "`X`" && Translate("`Button N`") == "`Y`",
        "face buttons by position");
  Check(Translate("`Shoulder R`") == "`R1`" && Translate("`Start`") == "`Menu`", "Z and Start");
  Check(Translate("`Trigger L`") == "`L2`" && Translate("`Trigger R`") == "`R2`", "analog triggers");
  Check(Translate("`Pad N`") == "`D-Pad Up`" && Translate("`Pad E`") == "`D-Pad Right`", "d-pad");
  Check(Translate("`Left Y+`") == "`Left Stick Y+`" && Translate("`Right X-`") == "`Right Stick X-`",
        "sticks, same sign");
  Check(Translate("`Button S` | `Pad S`") == "`A` | `D-Pad Down`", "an expression with two names");
  Check(Translate("`Unknown`") == "`Unknown`" && Translate("") == "", "unknown names unchanged");
  Check(Translate("`Button S") == "`Button S", "an unclosed quote is left as is");

  if (g_failures != 0)
    return EXIT_FAILURE;
  std::puts("deck_native_pad: all checks passed");
  return EXIT_SUCCESS;
}
