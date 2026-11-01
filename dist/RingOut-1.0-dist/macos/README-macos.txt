Ring Out - Ver 1.1  (macOS, Apple Silicon)
=======================

This package contains NO game data and NO game code. You supply a
GameCube disc image that you already have; Setup extracts it and
recompiles its executable on your Mac.

This first Mac build supports the US disc (SoulCalibur II, GRSEAF) only.
It needs a Mac with Apple Silicon (M1 or later) and macOS 13 or newer.

QUICK START
  1. Unzip this folder wherever you want to keep it.
  2. Right-click Setup.command > Open, then Open again in the dialog.
     (A plain double-click is refused the first time: Ring Out is not
     signed by Apple. See "UNSIGNED" below.)
  3. Choose your disc image (.iso / .gcm / .nkit.iso / .rvz).
  4. Setup recompiles it -- about 10 minutes, one time only.
  5. Start the game with RingOut.app.

  The first time, macOS may ask to install the "Command Line Tools".
  Say yes and run Setup again when it finishes: they are Apple's free
  C library headers, which the recompile needs and which no one else
  is allowed to redistribute.

UNSIGNED
  Ring Out has no Apple Developer ID, so macOS marks it "unidentified
  developer". Setup.command clears that mark (com.apple.quarantine) on
  this folder only, so RingOut.app and the tools inside open normally
  afterwards. If you prefer to do it yourself, in Terminal:
      xattr -dr com.apple.quarantine /path/to/this/folder

NETPLAY
  Netplay works Mac to Mac. A Mac and a PC cannot play each other yet:
  the two refuse at connect with a message saying so.

FILES
  RingOut.app   the game (runtime, MoltenVK for Vulkan on Metal)
  Setup.command double-click setup; runs setup.sh
  setup.sh      the setup itself (shared with the Linux package)
  tools/        the recompiler
  toolchain/    the compiler setup uses (LLVM clang, cmake, ninja)
  module-src/   sources and speed profiles the recompile builds from
  userdata/     your settings, saves and logs (launch.log)

Found a bug? Please report it, with userdata/launch.log attached:
  https://github.com/jackpoison-prog/RingOut/issues
