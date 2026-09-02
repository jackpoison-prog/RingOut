// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompGameData.h"

#include "Common/CommonPaths.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace RecompGameData
{
namespace
{
std::mutex s_mutex;
State s_state;
std::string s_user_directory;
std::string s_root_olk;

std::string OriginPath()
{
  return s_user_directory + "disc-origin.txt";
}

std::string CachePath()
{
  return s_user_directory + "game-data-hash.txt";
}

std::string RequestPath()
{
  return s_user_directory + "restore-game-data.request";
}

// Common::SHA1::DigestToString returns UPPERCASE hex; sha1sum, which setup.sh
// uses to record the baseline, writes lowercase. Comparing them raw made a
// completely untouched install report MODIFIED -- and that verdict turns
// netplay off, so the false positive was the expensive direction. Both sides
// are folded to one case rather than trusting either producer.
std::string NormalizeDigest(std::string digest)
{
  for (char& c : digest)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return digest;
}

// SHA-1 rather than SHA-256: this answers "did these bytes change", not
// "could someone forge these bytes", and Dolphin's SHA1 context is the one
// with a hardware-accelerated path already wired up. Streamed in blocks so a
// 590 MB file does not become 590 MB of resident memory.
std::string HashFile(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};

  auto context = Common::SHA1::CreateContext();
  std::vector<u8> buffer(1 << 20);
  while (file)
  {
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto read = static_cast<size_t>(file.gcount());
    if (read == 0)
      break;
    context->Update(std::span(buffer.data(), read));
  }
  return NormalizeDigest(Common::SHA1::DigestToString(context->Finish()));
}

struct Stamp
{
  std::uintmax_t size = 0;
  std::int64_t mtime = 0;
};

Stamp StampOf(const std::filesystem::path& path)
{
  std::error_code ec;
  Stamp stamp;
  stamp.size = std::filesystem::file_size(path, ec);
  const auto time = std::filesystem::last_write_time(path, ec);
  stamp.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
  return stamp;
}

// The cache exists only to skip the rehash, so anything unexpected in it means
// "hash again" rather than "trust it".
std::string ReadCachedHash(const Stamp& stamp)
{
  std::ifstream file(CachePath());
  if (!file)
    return {};
  std::uintmax_t size = 0;
  std::int64_t mtime = 0;
  std::string hash;
  if (!(file >> size >> mtime >> hash))
    return {};
  if (size != stamp.size || mtime != stamp.mtime)
    return {};
  return NormalizeDigest(std::move(hash));
}

void WriteCachedHash(const Stamp& stamp, const std::string& hash)
{
  std::ofstream file(CachePath(), std::ios::trunc);
  if (file)
    file << stamp.size << ' ' << stamp.mtime << ' ' << hash << '\n';
}
}  // namespace

void Initialize(const std::string& game_root, const std::string& user_directory)
{
  std::lock_guard lock(s_mutex);

  s_user_directory = user_directory;
  if (!s_user_directory.empty() && s_user_directory.back() != DIR_SEP_CHR)
    s_user_directory += DIR_SEP_CHR;

  s_state = State{};

  const std::filesystem::path olk =
      std::filesystem::path(game_root) / "files" / "root.olk";
  s_root_olk = olk.string();
  std::error_code ec;
  if (!std::filesystem::exists(olk, ec))
  {
    s_state.detail = "No root.olk under the game directory -- nothing to check.";
    return;
  }

  const Stamp stamp = StampOf(olk);
  s_state.current = ReadCachedHash(stamp);
  if (s_state.current.empty())
  {
    s_state.current = HashFile(olk.string());
    if (!s_state.current.empty())
      WriteCachedHash(stamp, s_state.current);
  }

  Common::IniFile origin;
  if (origin.Load(OriginPath()))
    origin.GetIfExists("Origin", "GameDataHash", &s_state.pristine);
  s_state.pristine = NormalizeDigest(std::move(s_state.pristine));

  std::string source;
  if (origin.GetIfExists("Origin", "SourceImage", &source) && !source.empty())
  {
    s_state.restore_source = source;
    s_state.restorable = std::filesystem::exists(std::filesystem::path(source), ec);
  }

  if (s_state.pristine.empty() || s_state.current.empty())
  {
    s_state.status = Status::Unknown;
    s_state.detail = "No baseline for this install. Re-run setup.sh to record one.";
  }
  else if (s_state.pristine == s_state.current)
  {
    s_state.status = Status::Pristine;
    s_state.detail = "Game data matches the disc it was extracted from.";
  }
  else
  {
    s_state.status = Status::Modified;
    s_state.detail = s_state.restorable
                         ? "root.olk differs from the disc. Netplay is off; "
                           "the original can be restored."
                         : "root.olk differs from the disc. Netplay is off, and the "
                           "disc image it came from is no longer where setup.sh found it.";
  }

  INFO_LOG_FMT(VIDEO, "[game-data] {} (current={} pristine={})", s_state.detail,
               s_state.current.empty() ? "?" : s_state.current,
               s_state.pristine.empty() ? "?" : s_state.pristine);
}

State Get()
{
  std::lock_guard lock(s_mutex);
  return s_state;
}

std::string RootOlkPath()
{
  std::lock_guard lock(s_mutex);
  return s_root_olk;
}

std::string Fingerprint()
{
  std::lock_guard lock(s_mutex);
  return s_state.current.empty() ? "unknown" : s_state.current;
}

bool NetplayBlocked()
{
  std::lock_guard lock(s_mutex);
  return s_state.status == Status::Modified;
}

bool RequestRestore(std::string* message)
{
  std::lock_guard lock(s_mutex);

  if (s_state.restore_source.empty())
  {
    if (message)
      *message = "No disc image recorded -- re-run setup.sh with your disc.";
    return false;
  }
  if (!s_state.restorable)
  {
    if (message)
      *message = "Disc image is gone from " + s_state.restore_source;
    return false;
  }

  std::ofstream file(RequestPath(), std::ios::trunc);
  if (!file)
  {
    if (message)
      *message = "Could not write the restore request.";
    return false;
  }
  file << s_state.restore_source << '\n';
  if (message)
    *message = "Original game data will be restored on the next launch.";
  return true;
}

bool RestoreRequested()
{
  std::lock_guard lock(s_mutex);
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(RequestPath()), ec);
}

void CancelRestore()
{
  std::lock_guard lock(s_mutex);
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(RequestPath()), ec);
}
}  // namespace RecompGameData
