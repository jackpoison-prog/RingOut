// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Guest characters -- the parts with no Dolphin dependency, so tests can reach them.
//
// Guests come from the player's own discs: Spawn (29) from the Xbox disc, Heihachi (28) from the PS2
// disc. Their data is a second archive, built by tools/guests/build_guests.py: the GC US root.olk plus
// each guest's block appended to group 4 and its ending art in record 2/(57 + ID). It lives OUTSIDE the
// disc tree, in <game>/guests/, because every file under <game>/files/ becomes a file on the virtual
// disc -- with it there, the disc's file table would change even with guests off.
//
// The manifest beside it is written last by the builder and is the only thing trusted: an archive
// is used only when its manifest says it was built from THIS root.olk (SHA-1 as RecompGameData
// computes it) for this disc, and its size matches. Anything else turns guests off and says why.

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <picojson.h>

namespace RecompGuestCore
{
constexpr std::uint64_t kNoFile = std::numeric_limits<std::uint64_t>::max();

// One guest's rows of the executable's per-character tables, converted from the player's own disc by
// the builder. Where they go, and the check before writing: RecompGuestTables.h.
struct GuestTables
{
  std::uint8_t id = 0;
  std::uint16_t block = 0;  // first group-4 record of the block in root-guests.olk
  std::uint8_t roster = 0;
  std::uint8_t voice = 0;  // 1 = English voice slots (34-36)
  std::vector<std::uint8_t> costume;     // 6 bytes
  std::vector<std::uint8_t> descriptor;  // 0xF8 bytes, GC byte order, pointers into the string area
  std::vector<std::uint8_t> strings;     // 1..the guest's room
  std::vector<std::uint8_t> cell;        // 36-byte select-grid cell record; starts with the ID
  // The ending row (illustration entry, pages, text index) and the bytes the DOL ships there (-1s).
  std::vector<std::uint8_t> ending, ending_old;  // 8 bytes each
  // A kerned name string padded to 36 bytes. Empty when the DOL already holds the guest's string.
  std::vector<std::uint8_t> name;
  // The weapon-select preview pose: 28-byte bone records (u32 parent, xyz, rotation), GC byte order.
  std::vector<std::uint8_t> preview;
};

struct Manifest
{
  std::string disc;
  std::string gc_root_sha1;
  std::string sha1;
  std::string guests_sha1;  // what netplay peers compare
  std::uint64_t size = 0;
  // Character pools (opponent lists) repacked with every guest, the pointer table that reaches them,
  // and the shipped bytes of both from the player's DOL (what the runtime checks before writing).
  std::vector<std::uint8_t> pool_block, pool_table, pool_block_old, pool_table_old;  // 0xF8, 0xAC
  std::vector<GuestTables> guests;
};

inline bool ParseHex(const std::string& text, std::vector<std::uint8_t>* out)
{
  if (text.size() % 2)
    return false;
  auto nib = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  out->clear();
  for (std::size_t i = 0; i < text.size(); i += 2)
  {
    const int hi = nib(text[i]), lo = nib(text[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out->push_back(static_cast<std::uint8_t>(hi * 16 + lo));
  }
  return true;
}

// Hex field `k` of object `o`, between `lo` and `hi` bytes.
inline bool ParseHexField(const picojson::object& o, const char* k, std::size_t lo, std::size_t hi,
                          std::vector<std::uint8_t>* out)
{
  auto it = o.find(k);
  return it != o.end() && it->second.is<std::string>() && ParseHex(it->second.get<std::string>(), out) &&
         out->size() >= lo && out->size() <= hi;
}

constexpr std::size_t kMaxStrings = 0x80, kPreviewRecord = 28, kMaxPreview = 2 * kPreviewRecord;

inline bool ParseGuest(const picojson::value& v, GuestTables* t)
{
  if (!v.is<picojson::object>())
    return false;
  const picojson::object& o = v.get<picojson::object>();
  auto num = [&](const char* k, double lo, double hi, double* d) {
    auto it = o.find(k);
    if (it == o.end() || !it->second.is<double>())
      return false;
    *d = it->second.get<double>();
    return *d >= lo && *d <= hi && *d == static_cast<double>(static_cast<long long>(*d));
  };
  auto hex = [&](const char* k, std::size_t lo, std::size_t hi, std::vector<std::uint8_t>* out) {
    return ParseHexField(o, k, lo, hi, out);
  };
  double id = 0, block = 0, roster = 0, voice = 0;
  if (!num("id", 28, 29, &id) || !num("block", 1, 65535, &block) || !num("roster", 0, 255, &roster) ||
      !num("voice", 0, 1, &voice) || !hex("costume", 6, 6, &t->costume) ||
      !hex("descriptor", 0xF8, 0xF8, &t->descriptor) || !hex("strings", 1, id == 29 ? 0x38 : kMaxStrings, &t->strings) ||
      !hex("cell", 36, 36, &t->cell) || t->cell[0] != 0 || t->cell[1] != id ||
      !hex("ending", 8, 8, &t->ending) || !hex("ending_old", 8, 8, &t->ending_old) ||
      !hex("preview", kPreviewRecord, kMaxPreview, &t->preview) || t->preview.size() % kPreviewRecord != 0)
    return false;
  if (id == 29 && !hex("name", 36, 36, &t->name))  // only Spawn: the DOL still holds Heihachi's string
    return false;
  t->id = static_cast<std::uint8_t>(id);
  t->block = static_cast<std::uint16_t>(block);
  t->roster = static_cast<std::uint8_t>(roster);
  t->voice = static_cast<std::uint8_t>(voice);
  return true;
}

enum class Check
{
  Ok,
  BadManifest,   // missing, unreadable or an unknown format
  OldFormat,     // a Spawn-only build from before the guest list
  WrongDisc,     // built for another region
  BaseMismatch,  // built from a different root.olk (e.g. one with skins installed since)
  NoArchive,
  SizeMismatch,  // archive cut short or replaced
};

inline const char* Describe(Check c)
{
  switch (c)
  {
  case Check::Ok: return "ok";
  case Check::BadManifest: return "no readable guests/guests.json";
  case Check::OldFormat: return "this Spawn install is from an older version (rebuild guests: rerun setup)";
  case Check::WrongDisc: return "guest data was built for a different disc";
  case Check::BaseMismatch: return "root.olk changed since the guests were built (rebuild guests)";
  case Check::NoArchive: return "guests/root-guests.olk is missing";
  case Check::SizeMismatch: return "guests/root-guests.olk is incomplete";
  }
  return "?";
}

inline std::string ManifestPath(const std::string& game_root) { return game_root + "/guests/guests.json"; }
inline std::string ArchivePath(const std::string& game_root) { return game_root + "/guests/root-guests.olk"; }
// Where a Spawn-only install from before the guest list keeps its manifest.
inline std::string OldManifestPath(const std::string& game_root) { return game_root + "/spawn/root-spawn.json"; }
inline std::string RootOlkPath(const std::string& game_root) { return game_root + "/files/root.olk"; }

// `root_sha1`: RecompGameData's hash of the installed root.olk. `archive_size`: the archive's size,
// or kNoFile when it does not exist.
inline Check Validate(const std::string& manifest_text, const std::string& root_sha1,
                      std::uint64_t archive_size, Manifest* out)
{
  picojson::value v;
  if (manifest_text.empty() || !picojson::parse(v, manifest_text).empty() || !v.is<picojson::object>())
    return Check::BadManifest;
  const picojson::object& o = v.get<picojson::object>();
  auto str = [&](const char* k) -> const std::string* {
    auto it = o.find(k);
    return it != o.end() && it->second.is<std::string>() ? &it->second.get<std::string>() : nullptr;
  };
  auto num = [&](const char* k, double* d) {
    auto it = o.find(k);
    if (it == o.end() || !it->second.is<double>())
      return false;
    *d = it->second.get<double>();
    return true;
  };
  double format = 0, size = 0;
  const std::string *disc = str("disc"), *base = str("gc_root_sha1"), *sha1 = str("sha1"),
                    *hash = str("guests_sha1");
  if (num("format", &format) && format == 1)
    return Check::OldFormat;
  if (format != 2 || !num("size", &size) || !disc || !base || !sha1 || !hash)
    return Check::BadManifest;
  Manifest m;
  if (!ParseHexField(o, "pool_block", 0xF8, 0xF8, &m.pool_block) ||
      !ParseHexField(o, "pool_table", 0xAC, 0xAC, &m.pool_table) ||
      !ParseHexField(o, "pool_block_old", 0xF8, 0xF8, &m.pool_block_old) ||
      !ParseHexField(o, "pool_table_old", 0xAC, 0xAC, &m.pool_table_old))
    return Check::BadManifest;
  auto g = o.find("guests");
  if (g == o.end() || !g->second.is<picojson::array>() || g->second.get<picojson::array>().empty())
    return Check::BadManifest;
  for (const picojson::value& entry : g->second.get<picojson::array>())
  {
    GuestTables t;
    if (!ParseGuest(entry, &t))
      return Check::BadManifest;
    for (const GuestTables& seen : m.guests)
      if (seen.id == t.id)
        return Check::BadManifest;
    m.guests.push_back(std::move(t));
  }
  if (*disc != "GRSEAF")
    return Check::WrongDisc;
  if (*base != root_sha1)
    return Check::BaseMismatch;
  if (archive_size == kNoFile)
    return Check::NoArchive;
  if (static_cast<std::uint64_t>(size) != archive_size)
    return Check::SizeMismatch;
  m.disc = *disc;
  m.gc_root_sha1 = *base;
  m.sha1 = *sha1;
  m.guests_sha1 = *hash;
  m.size = archive_size;
  *out = std::move(m);
  return Check::Ok;
}

// The host file that backs a disc file: the archive in place of <game_root>/files/root.olk while
// guests are on, the file itself otherwise. Paths compare after lexical normalisation, so "//" and "./"
// in either do not matter, and only the game's OWN top-level root.olk ever switches.
inline std::string Resolve(const std::string& host_path, bool on, const std::string& game_root)
{
  if (!on)
    return host_path;
  // Compare absolute paths: `--game game-sp9` is relative while the disc may open an absolute path
  // (or the reverse). A miss serves the stock archive under the guests' tables, and the select screen hangs.
  std::error_code ec;
  const auto abs = [&ec](const std::string& p) {
    const std::filesystem::path a = std::filesystem::absolute(p, ec);
    return (ec ? std::filesystem::path(p) : a).lexically_normal();
  };
  const std::filesystem::path root = abs(game_root);
  if (abs(host_path) != (root / "files" / "root.olk").lexically_normal())
    return host_path;
  return (root / "guests" / "root-guests.olk").lexically_normal().string();
}
}  // namespace RecompGuestCore
