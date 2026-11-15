#include "netplay_compatibility.hpp"

#include "Core/RecompGuest.h"

#include "Common/Crypto/SHA1.h"
#include "Common/Version.h"
#include "VideoCommon/RecompGameData.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/module_loader.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace moderngekko::frontend {
namespace {
void HashU32(Common::SHA1::Context &context, std::uint32_t value) {
  const std::array<u8, 4> bytes = {
      static_cast<u8>(value), static_cast<u8>(value >> 8),
      static_cast<u8>(value >> 16), static_cast<u8>(value >> 24)};
  context.Update(bytes);
}

void HashU64(Common::SHA1::Context &context, std::uint64_t value) {
  const std::array<u8, 8> bytes = {
      static_cast<u8>(value),       static_cast<u8>(value >> 8),
      static_cast<u8>(value >> 16), static_cast<u8>(value >> 24),
      static_cast<u8>(value >> 32), static_cast<u8>(value >> 40),
      static_cast<u8>(value >> 48), static_cast<u8>(value >> 56)};
  context.Update(bytes);
}

void HashRanges(Common::SHA1::Context &context, const ModernGekkoRange *ranges,
                std::uint32_t count) {
  HashU32(context, count);
  for (std::uint32_t i = 0; i < count; ++i) {
    HashU32(context, ranges[i].start);
    HashU32(context, ranges[i].end);
  }
}

std::string DescriptorFingerprint(const ModernGekkoModuleDesc &descriptor) {
  auto context = Common::SHA1::CreateContext();
  HashU32(*context, descriptor.abi_version);
  HashU32(*context, descriptor.cpu_abi_version);
  HashU32(*context, descriptor.cpu_state_size);
  context->Update(std::span(reinterpret_cast<const u8 *>(descriptor.game_id),
                            sizeof(descriptor.game_id)));
  HashU32(*context, descriptor.entry_point);
  HashU32(*context, descriptor.on_state_loaded ? 1 : 0);
  HashRanges(*context, descriptor.code_ranges, descriptor.num_code_ranges);
  HashRanges(*context, descriptor.smc_ranges, descriptor.num_smc_ranges);
  HashRanges(*context, descriptor.chunk_ranges, descriptor.num_chunk_ranges);
  for (std::uint32_t i = 0; i < descriptor.num_chunk_ranges; ++i)
    HashU64(*context, descriptor.chunk_hashes[i]);
  return Common::SHA1::DigestToString(context->Finish());
}

#if defined(_WIN32)
std::vector<std::unique_ptr<ModuleLibrary>> &PinnedModuleLibraries() {
  static auto *libraries = new std::vector<std::unique_ptr<ModuleLibrary>>();
  return *libraries;
}
#endif
} // namespace

const char *HostPlatformName() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#else
  return "x86_64";
#endif
}

std::string CompatibilityFingerprint(const RuntimeConfig &config,
                                     const GameMetadata &game) {
  std::string module = "interpreter";
  bool guest_sites = false;  // the module has every code site the installed guests need
  if (config.module.kind != ModuleSource::Kind::None) {
    const ModernGekkoModuleRequirements requirements = {
        MODERNGEKKO_CPU_ABI_VERSION,
        static_cast<std::uint32_t>(sizeof(CPUState)), game.disc_id.c_str()};
    auto library = std::make_unique<ModuleLibrary>();
    ModuleLoadResult loaded;
    if (config.module.kind == ModuleSource::Kind::DynamicPath) {
      loaded = library->Open(config.module.path.string(), requirements);
    } else {
      loaded = library->Attach(config.module.descriptor, requirements);
    }
    if (loaded.status == ModuleLoadStatus::Ok) {
      module = DescriptorFingerprint(*library->GetDescriptor());
      const auto* groups =
          static_cast<const std::uint32_t*>(library->GetSymbol("staticrecomp_gate_groups"));
      guest_sites = RecompGuest::ModuleRunsGuests(groups ? *groups : 0u);
#if defined(_WIN32)
      if (config.module.kind == ModuleSource::Kind::DynamicPath)
        PinnedModuleLibraries().push_back(std::move(library));
#endif
    } else {
      module = "rejected";
    }
  }
  // Bumped to -7 because the fingerprint gained a field. root.olk is in it
  // now: a GameBanana character skin rewrites that archive and leaves main.dol
  // untouched, so on -6 two peers with different skins matched on every field
  // here and then desynced in play with nothing to explain it. Its contents
  // reach guest RAM, so they are part of what has to agree.
  // -8: the platform is the SECOND field, where the server looks for it to
  // refuse a Mac joining a PC with that reason instead of "differently built".
  // -9: the guest characters. With them on, one peer's RAM gets their select cells and character
  // pools and its disc serves their archive in place of root.olk, so peers must agree on whether
  // guests are on AND on which build of them (the same guests from different discs differ).
  const char* const guests_env = std::getenv("RINGOUT_GUESTS");
  const bool guests_on =
      RecompGuest::IsAvailable() && guest_sites && !(guests_env && guests_env[0] == '0');
  const std::string guests = guests_on ? "guests:" + RecompGuest::GuestsHash() : "noguests";
  return std::string("moderngekko-netplay-9|") + HostPlatformName() + "|" +
         Common::GetScmRevGitStr() + "|" +
         game.disc_id + "|" + game.dol_sha256 + "|" +
         RecompGameData::Fingerprint() + "|" +
         std::to_string(MODERNGEKKO_MODULE_ABI_VERSION) + "|" +
         std::to_string(MODERNGEKKO_CPU_ABI_VERSION) + "|" +
         std::to_string(sizeof(CPUState)) + "|" + module + "|" + guests;
}
} // namespace moderngekko::frontend
