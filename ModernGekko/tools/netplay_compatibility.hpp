#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"

#include <string>

namespace moderngekko::frontend {
// "x86_64" or "arm64": the same module source rounds floating point
// differently on the two, so it is part of what two netplay peers must share.
const char *HostPlatformName();
std::string CompatibilityFingerprint(const RuntimeConfig &config,
                                     const GameMetadata &game);
}
