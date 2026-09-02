// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompOlk.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace RecompOlk
{
namespace
{
constexpr std::uint64_t kHeaderSize = 2048;
constexpr std::uint32_t kMagic = 0x6b6e6c6f;  // "olnk"

std::uint32_t ReadLE32(const unsigned char* p)
{
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Reads one container's index. `header_size` differs per level: the root uses
// 2048, and the nested containers a larger one given by their own first record.
std::vector<std::pair<std::uint32_t, std::uint32_t>> ReadIndex(std::ifstream& file,
                                                               std::uint64_t base,
                                                               std::uint64_t header_size,
                                                               std::uint64_t* data_start)
{
  std::vector<unsigned char> hdr(header_size);
  file.seekg(static_cast<std::streamoff>(base));
  file.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(header_size));
  if (!file)
    return {};
  if (ReadLE32(hdr.data() + 4) != kMagic)
    return {};

  const std::uint32_t count = ReadLE32(hdr.data());
  // Record 1 is the data-region descriptor; its offset is where entries live.
  if (header_size < 32)
    return {};
  *data_start = base + ReadLE32(hdr.data() + 16);

  std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    const std::uint64_t at = 32 + static_cast<std::uint64_t>(i) * 16;
    if (at + 16 > header_size)
      break;
    entries.emplace_back(ReadLE32(hdr.data() + at), ReadLE32(hdr.data() + at + 4));
  }
  return entries;
}
}  // namespace

Slot FindUniqueSlotBySize(const std::string& root_olk, std::uint32_t size)
{
  Slot result;
  if (size == 0)
    return result;

  std::ifstream file(root_olk, std::ios::binary);
  if (!file)
    return result;

  std::uint64_t top_data = 0;
  const auto top = ReadIndex(file, 0, kHeaderSize, &top_data);

  // Walk every top-level container. Skins live in the big one, but searching
  // all of them costs nothing and avoids hard-coding which that is.
  int matches = 0;
  for (const auto& [off, sz] : top)
  {
    if (sz == 0)
      continue;
    const std::uint64_t base = top_data + off;

    // The nested container's own header size comes from its first record.
    std::vector<unsigned char> probe(32);
    file.seekg(static_cast<std::streamoff>(base));
    file.read(reinterpret_cast<char*>(probe.data()), 32);
    if (!file || ReadLE32(probe.data() + 4) != kMagic)
      continue;
    const std::uint64_t nested_header = ReadLE32(probe.data() + 16);
    if (nested_header < 32 || nested_header > (1u << 20))
      continue;

    std::uint64_t data_start = 0;
    const auto entries = ReadIndex(file, base, nested_header, &data_start);
    for (const auto& [eoff, esz] : entries)
    {
      if (esz != size)
        continue;
      ++matches;
      result.found = true;
      result.offset = data_start + eoff;
      result.size = esz;
    }
  }

  if (matches != 1)
    return Slot{};
  return result;
}

bool SlotMatchesFile(const std::string& root_olk, const Slot& slot, const std::string& path)
{
  if (!slot.found)
    return false;
  std::ifstream olk(root_olk, std::ios::binary);
  std::ifstream mod(path, std::ios::binary);
  if (!olk || !mod)
    return false;
  olk.seekg(static_cast<std::streamoff>(slot.offset));

  std::vector<char> a(64 * 1024), b(64 * 1024);
  std::uint32_t left = slot.size;
  while (left > 0)
  {
    const std::streamsize chunk = static_cast<std::streamsize>(std::min<std::uint32_t>(left, 64 * 1024));
    olk.read(a.data(), chunk);
    mod.read(b.data(), chunk);
    if (olk.gcount() != chunk || mod.gcount() != chunk)
      return false;
    if (std::memcmp(a.data(), b.data(), static_cast<size_t>(chunk)) != 0)
      return false;
    left -= static_cast<std::uint32_t>(chunk);
  }
  return true;
}
}  // namespace RecompOlk
