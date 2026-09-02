// SPDX-License-Identifier: GPL-2.0-or-later
//
// RingOut: just enough of the root.olk container to place a character skin.
//
// Format, reverse-engineered from the shipped disc: little-endian, magic "olnk"
// at offset 4, entry count at 0, a 2048-byte header, then 16-byte records of
// (offset, size, ?, 0). Record 1 describes the whole data region; real entries
// start at record 2, and their offsets are relative to the container's data
// start rather than the file. Note the endianness: the index is LITTLE-endian
// even though this is a PowerPC console's disc, and the payloads inside are
// big-endian. Nothing is compressed -- entries sit raw at fixed extents.
//
// The consequence that shapes this whole feature: an entry has a FIXED extent,
// so a skin can only be dropped in when its size matches the slot exactly. A
// different size means every later offset moves and the parent header needs
// fixing up -- a real container rebuild, which this deliberately does not do.

#pragma once

#include <cstdint>
#include <string>

namespace RecompOlk
{
struct Slot
{
  bool found = false;
  std::uint64_t offset = 0;  // absolute byte offset into root.olk
  std::uint32_t size = 0;
};

// The one entry in root.olk whose size equals `size`, if exactly one exists.
// Ambiguity is treated as "no": placing a skin in the wrong costume slot is
// worse than declining to place it.
Slot FindUniqueSlotBySize(const std::string& root_olk, std::uint32_t size);

// Do the bytes at that slot already equal the file's contents?
bool SlotMatchesFile(const std::string& root_olk, const Slot& slot, const std::string& path);
}  // namespace RecompOlk
