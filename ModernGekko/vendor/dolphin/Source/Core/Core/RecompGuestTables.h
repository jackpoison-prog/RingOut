// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Guest rows in the US executable's per-character tables (tools/spawn/MAP.md, "Per-character tables").
// The GC DOL keeps slots for characters 28 and 29 in each table and leaves them empty; the bytes that
// fill them come from the player's own discs, through the manifest, never from this source.
//
// Every slot is checked against what this DOL ships before anything is written, and the writes are all
// or nothing: another build of the game, or RAM someone else already changed, gets no writes at all.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "RecompGuestCore.h"

namespace RecompGuestTables
{
constexpr std::uint32_t kDescAddr = 0x80275A60, kDescStride = 0xF8;  // 0xF8-byte descriptor per ID
constexpr std::uint32_t kCostumeAddr = 0x80279484;  // 6 bytes per ID, read by the check at 800A8E04
constexpr std::uint32_t kRosterAddr = 0x80250958;   // 1 byte per ID
constexpr std::uint32_t kBlockAddr = 0x80277A58;    // s16 per ID: first group-4 record of the block
// His extra data lives in the all-zero descriptor rows of IDs 7 and 8, in bytes that are not pointer
// slots. Row 28 and the shipped name strings of 28 and 29 stay untouched: ID 28 is Heihachi's.
constexpr std::uint32_t kSpawnHome = kDescAddr + 7 * kDescStride;
constexpr std::uint32_t kStringsAddr = kSpawnHome + 0xC0, kStringsSize = 0x38;
// Link's model ID in his descriptor: 27 once the DOL is in RAM, before then the apploader's bytes.
constexpr std::uint32_t kLinkIdField = kDescAddr + 27 * kDescStride + 0x86;
constexpr std::uint32_t kSpawnId = 29, kHeihachiId = 28;
// Select grid: 36-byte cell records, rows of 4 plus a -1 end marker (index = row * 5 + col), set up by
// the code at 80149B60. Spawn's cell is the free 5th slot of Link's row, cell 4, drawn right of the frame;
// Heihachi's is the free 5th slot of the next row, cell 9.
constexpr std::uint32_t kCellAddr = 0x80250520, kCellStride = 36, kSpawnCell = 4, kHeihachiCell = 9;
// Character pools: 255-terminated ID lists (opponent pools) in one block, reached through a table of
// 43 pointers. GC lists carry Link (27) where the Xbox's carry Spawn; the manifest's block adds 29
// beside 27 and its table follows the lists to their new places (tools/spawn/tables.py).
constexpr std::uint32_t kPoolAddr = 0x8028B50C, kPoolSize = 0xF8;
constexpr std::uint32_t kPoolTableAddr = 0x8028B604, kPoolTableSize = 43 * 4;
// Endings, 8 bytes per ID: s16 illustration entry (group 2), s16 page count, u32 text index. GC ships
// -1s for 29; the ending then asks the size of entry -1, gets the whole group's, and its next buffer
// lands past the end of RAM (a crash after the final fight). His row is the Xbox's (tables.py).
constexpr std::uint32_t kEndingAddr = 0x802A3748, kEndingStride = 8;
// English voice, a byte per ID: when set and the voice language is English, 80098A04 moves a voice
// slot from 30-32 to 34-36. GC ships 0 for 29 (the dummy), so he spoke Japanese; the Xbox has 1.
constexpr std::uint32_t kVoiceAddr = 0x80277A9C;
// Names, a pointer per ID to the kerned string the VS screen and HUD draw. GC 29 points at the dummy's
// "Q.TAKAGI" (0x8028ED7C). His Xbox string goes in empty descriptor row 8 and pointer 29 moves to it.
constexpr std::uint32_t kNameTableAddr = 0x8028E9D8, kNameOld = 0x8028ED7C;
constexpr std::uint32_t kNameAt = kDescAddr + 8 * kDescStride + 0xC0;
constexpr std::size_t kNameRoom = 36;
// Weapon-select preview pose, per ID {u32 count, u32 pointer} to 28-byte bone records that 8017E998
// applies to the preview model each frame. GC ships count 0 for 29, so the preview kept the matrices its
// setup parks at y = 100 (800A3D98) and his weapon never showed. His Xbox record goes in 28 unused bytes
// of empty descriptor row 7 (no pointer slots, clear of the string area at +0xC0).
constexpr std::uint32_t kPreviewTable = 0x80251904, kPreviewHome = kSpawnHome + 0x64;
constexpr std::size_t kPreviewRec = 28;

using ReadFn = std::function<std::uint8_t(std::uint32_t)>;
using WriteFn = std::function<void(std::uint32_t, std::uint8_t)>;

struct SpawnPatch
{
  std::uint32_t addr;
  std::vector<std::uint8_t> expect;  // what the shipped DOL holds there
  std::vector<std::uint8_t> bytes;
  const char* what;
};

inline std::vector<std::uint8_t> Be32(std::uint32_t v)
{
  return {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16),
          static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
}

inline std::vector<std::uint8_t> CellEndMarker()
{
  std::vector<std::uint8_t> v(kCellStride, 0);
  v[0] = v[1] = 0xff;
  return v;
}

// Where each guest's rows go. ID 28 keeps his shipped name string and his own descriptor row, so his
// other data goes in empty rows too: strings in row 9 from +0x64 (no pointer slots from there on), the
// two preview records in the tail of row 10.
constexpr std::uint32_t kHeihachiStrings = kDescAddr + 9 * kDescStride + 0x64;
constexpr std::uint32_t kHeihachiStringsSize = 0x80;
constexpr std::uint32_t kHeihachiPreview = kDescAddr + 10 * kDescStride + 0xC0;

struct GuestSlot
{
  std::uint32_t id;
  std::uint32_t cell;
  std::uint32_t code;  // past the game's 24, see "Character codes" below
  std::uint32_t strings_addr, strings_size;
  std::uint32_t preview_addr;
};
inline constexpr GuestSlot kGuests[] = {
    {kSpawnId, kSpawnCell, 24, kStringsAddr, kStringsSize, kPreviewHome},
    {kHeihachiId, kHeihachiCell, 25, kHeihachiStrings, kHeihachiStringsSize, kHeihachiPreview},
};

inline const GuestSlot* FindGuest(std::uint32_t id)
{
  for (const GuestSlot& g : kGuests)
    if (g.id == id)
      return &g;
  return nullptr;
}

// Which guests this archive holds.
struct Installed
{
  bool spawn = false, heihachi = false;
  bool Has(std::uint32_t id) const { return (id == kSpawnId && spawn) || (id == kHeihachiId && heihachi); }
  bool Any() const { return spawn || heihachi; }
};

inline Installed InstalledFrom(const RecompGuestCore::Manifest& m)
{
  Installed in;
  for (const auto& t : m.guests)
    (t.id == kSpawnId ? in.spawn : in.heihachi) = true;
  return in;
}

inline std::vector<std::uint8_t> PreviewRow(std::uint32_t count, std::uint32_t home)
{
  std::vector<std::uint8_t> v = Be32(count), p = Be32(home);
  v.insert(v.end(), p.begin(), p.end());
  return v;
}

inline void AddGuestPatches(const RecompGuestCore::GuestTables& t, std::vector<SpawnPatch>* out)
{
  const GuestSlot& g = *FindGuest(t.id);
  std::vector<std::uint8_t> strings = t.strings;
  strings.resize(g.strings_size, 0);
  const std::vector<SpawnPatch> rows{
      {kBlockAddr + g.id * 2, {0xff, 0xff},
       {static_cast<std::uint8_t>(t.block >> 8), static_cast<std::uint8_t>(t.block)}, "block slot"},
      // ID -> select cell. The manifest carries the other disc's value (Spawn's 1 is Link's cell on GC);
      // on GC each guest has a cell of his own, and every ID -> cell lookup has to find it (the ranking's face).
      {kRosterAddr + g.id, {0x13}, {static_cast<std::uint8_t>(g.cell)}, "roster slot"},
      {kCostumeAddr + g.id * 6, std::vector<std::uint8_t>(6, 0), t.costume, "costume slot"},
      {g.strings_addr, std::vector<std::uint8_t>(g.strings_size, 0), strings, "string area"},
      {kDescAddr + g.id * kDescStride, std::vector<std::uint8_t>(kDescStride, 0), t.descriptor,
       "descriptor slot"},
      {kCellAddr + g.cell * kCellStride, CellEndMarker(), t.cell, "select cell"},
      {kEndingAddr + g.id * kEndingStride, t.ending_old, t.ending, "ending row"},
      {kVoiceAddr + g.id, {0}, {t.voice}, "English-voice flag"},
  };
  out->insert(out->end(), rows.begin(), rows.end());
  if (!t.name.empty())
  {
    out->push_back({kNameAt, std::vector<std::uint8_t>(kNameRoom, 0), t.name, "name string"});
    out->push_back({kNameTableAddr + g.id * 4, Be32(kNameOld), Be32(kNameAt), "name pointer"});
  }
  out->push_back({g.preview_addr, std::vector<std::uint8_t>(t.preview.size(), 0), t.preview, "preview pose"});
  out->push_back({kPreviewTable + g.id * 8, std::vector<std::uint8_t>(8, 0),
                  PreviewRow(static_cast<std::uint32_t>(t.preview.size() / kPreviewRec), g.preview_addr),
                  "preview row"});
}

inline std::vector<SpawnPatch> MakeUsPatches(const RecompGuestCore::Manifest& m)
{
  std::vector<SpawnPatch> out{
      {kPoolAddr, m.pool_block_old, m.pool_block, "character pools"},
      {kPoolTableAddr, m.pool_table_old, m.pool_table, "pool table"},
  };
  for (const auto& t : m.guests)
    AddGuestPatches(t, &out);
  return out;
}

enum class PatchResult
{
  Applied,
  AlreadyApplied,  // every slot already holds his bytes (a savestate taken with him on)
  Mismatch,        // some slot holds neither: not the build these addresses came from
  NotReady,        // the DOL is not in RAM yet
};

struct ApplyResult
{
  PatchResult result;
  const char* what = "";  // the first slot that matched neither, for Mismatch
};

inline bool Holds(const SpawnPatch& p, const std::vector<std::uint8_t>& v, const ReadFn& read)
{
  for (std::size_t i = 0; i < v.size(); ++i)
    if (read(p.addr + static_cast<std::uint32_t>(i)) != v[i])
      return false;
  return true;
}

inline ApplyResult ApplyPatches(const std::vector<SpawnPatch>& patches, const ReadFn& read, const WriteFn& write)
{
  if (read(kLinkIdField) != 0 || read(kLinkIdField + 1) != 27)
    return {PatchResult::NotReady};
  bool all_done = true;
  for (const auto& p : patches)
  {
    if (Holds(p, p.bytes, read))
      continue;
    all_done = false;
    if (!Holds(p, p.expect, read))
      return {PatchResult::Mismatch, p.what};
  }
  if (all_done)
    return {PatchResult::AlreadyApplied};
  for (const auto& p : patches)
    for (std::size_t i = 0; i < p.bytes.size(); ++i)
      write(p.addr + static_cast<std::uint32_t>(i), p.bytes[i]);
  return {PatchResult::Applied};
}

// Once per boot: retried every frame until the DOL is in RAM, then applied (or refused) exactly once.
class BootPatcher
{
public:
  // The frame's result while it is still trying or on the frame it finishes; nothing once done or off.
  const ApplyResult* OnFrame(bool on, const std::vector<SpawnPatch>& patches, const ReadFn& read,
                             const WriteFn& write)
  {
    if (!on || m_done)
      return nullptr;
    m_last = ApplyPatches(patches, read, write);
    m_done = m_last.result != PatchResult::NotReady;
    return &m_last;
  }
  bool Done() const { return m_done; }
  bool Applied() const
  {
    return m_done && (m_last.result == PatchResult::Applied || m_last.result == PatchResult::AlreadyApplied);
  }
  void Reset() { m_done = false; }

private:
  bool m_done = false;
  ApplyResult m_last{PatchResult::NotReady};
};

// Per-character code sites (tools/spawn/MAP.md, "Per-character code sites"). Each PC is a compare
// whose register holds a character ID: a jump-table bound for switches whose cases stop at Link
// (27), where 29 falls to the default. The module gates them (--gated-pc <pc>@1); while Spawn is
// on the run loop calls ApplySite there, which gives this one decision the case of `as_id`.
struct SpawnSite
{
  std::uint32_t pc;
  std::uint32_t guest_id;  // the ID this row is for; a PC can have one row per guest
  std::uint32_t as_id;
  const char* what;
};

inline constexpr std::array<SpawnSite, 5> kUsSites{{
    {0x800834DC, kSpawnId, 24, "504-510 switch (Xbox: his case is 24/25's)"},
    {0x80083594, kSpawnId, 24, "504-510 switch, second copy"},
    // as_id 29: his own case, the switch default (0 -> group-4 records 7/23, which the build fills
    // with his converted Xbox scene 17/33). The site stays because the module is gated here.
    {0x800B5628, kSpawnId, 29, "home stage: his own scene in records 7/23 (Xbox 17/33)"},
    {0x800834DC, kHeihachiId, 24, "504-510 switch (PS2: his case is 24/25's)"},
    {0x80083594, kHeihachiId, 24, "504-510 switch, second copy"},
}};

inline bool IsSitePc(std::uint32_t pc)
{
  for (const SpawnSite& s : kUsSites)
    if (s.pc == pc)
      return true;
  return false;
}

// The row for this PC and the ID in the compared register, if any.
inline const SpawnSite* FindSite(std::uint32_t pc, std::uint32_t id)
{
  for (const SpawnSite& s : kUsSites)
    if (s.pc == pc && s.guest_id == id)
      return &s;
  return nullptr;
}

// `insn` is the guest instruction at the site. Only a cmpwi/cmplwi whose register holds an installed
// guest's ID is touched: anything else means another DOL or another fighter, and is left alone.
inline bool ApplySite(const SpawnSite& site, const Installed& installed, std::uint32_t insn, std::uint32_t* gpr)
{
  const std::uint32_t op = insn >> 26;
  if (op != 10 && op != 11)
    return false;
  const std::uint32_t ra = (insn >> 16) & 31;
  if (gpr[ra] != site.guest_id || !installed.Has(site.guest_id))
    return false;
  gpr[ra] = site.as_id;
  return true;
}

// Character codes (tools/spawn/MAP.md, "Character codes"). Rankings and the per-character stats file
// a fighter under a code 0-23: its index in the 24-entry s16 ID table at 0x80274D10. ID -> code
// (800914C4) scans that table; 29 is not in it and there is no 25th slot, so for him it gives -1.
// Unhooked, that cost two things: his ranking records showed another fighter (the record keeps the
// code byte, and code -> ID at 80091494 clamps), and the stats writers indexed their 28-byte rows
// with -1 and overwrote the words in front of the table (in RAM: ...09 0b 0a 08 -> 00 01 86 9f).
//
// The hooks, all gated (group 1; Heihachi's six group 2) and live only while guests are on:
//  - a guest's ranking records get his code from kGuests (24 and 25, past the table), which reads back
//    as his ID while he is installed, so the ranking draws his name and face from the tables the select
//    screen already uses. Without him a saved row stays out of range and the game's clamp shows someone else;
//  - a board has one row per fighter, 20 in all, so none is his. 8011D3D4 looks the fighter's code up
//    in the first r28 rows and answers -1 (no rank, no name entry) when r29, the row found, reaches
//    r28. Pointing r29 at the last row instead makes it this fighter's row: the game's own compare
//    and shift then enter the time if it beats that row, and drop whoever held it. Whoever is dropped
//    gets back the same way. The function runs twice per clear (r27 != 0 only asks for the rank, then
//    name entry, then r27 == 0 writes), and both calls take this hook;
//  - the two stats writers skip any row whose code is outside 0-23. He has no row (the CHARACTER
//    DATA screen lists 24), and the fighters he plays against still get theirs.
constexpr std::uint32_t kCodeTable = 0x80274D10, kCodeCount = 24, kSpawnCode = kGuests[0].code;
// The stats object: per player the code (0x2B8 + slot * 4), the mode word (0x2B0), the dirty flags
// (0x2C8). Row r holds 7 words at r * 28; counters stop at 99999.
constexpr std::uint32_t kStatsCode = 0x2B8, kStatsMode = 0x2B0, kStatsFlags = 0x2C8, kStatsRow = 28;
constexpr std::uint32_t kStatsCap = 99999;

enum class HookKind
{
  CodeToId,       // 80091494 entry: a guest's code returns his ID
  RecordCodeReg,  // after the lookup: -1 for the ID in the low byte of `reg` becomes the guest's code
  RecordCodeMem,  // the same, for the ID in the byte `reg` points at
  PlayCount,      // 80178BD4 (obj r3, slot r4, ID r5): for a code-less ID, store -1 and skip the row
  WinLoss,        // 80178C50 (obj r3, winner slot r4 or -1): count only rows with a real code
  RowMissing,     // 8011D574: the board has no row for this fighter, so the last row is theirs to beat
  HomeScene,      // 800B5628 (ID r0): Heihachi's scene value. Records 18/34, which no GC case reaches
  ForearmMount,   // 80032E44 (model r25, frame r1): Heihachi's gauntlets on his forearms, as the PS2 hangs them
  CellPlace,      // after a cell's position is stored at gpr[reg] + off: his cell moves left of Mitsurugi
  CellExit,       // 801521B8 (tile r28, column r0): his tile leaves the screen as column 0
  CellMove,       // 801530A4 (cursor r26, buttons r28, new cell r29, old r31): the cursor reaches him there
};

struct SpawnHook
{
  std::uint32_t pc;
  HookKind kind;
  std::uint32_t reg;
  const char* what;
  std::uint32_t off = 0;
};

inline constexpr std::array<SpawnHook, 13> kUsHooks{{
    {0x80091494, HookKind::CodeToId, 3, "code -> ID: a guest's code is him"},
    {0x8011D444, HookKind::RecordCodeReg, 19, "ranking entry: a guest's code"},
    {0x8011D574, HookKind::RowMissing, 0, "ranking board: no row, the last row is at stake"},
    {0x8011DBE8, HookKind::RecordCodeMem, 19, "team ranking member: a guest's code"},
    {0x80178BD4, HookKind::PlayCount, 0, "play count: no row for him"},
    {0x80178C50, HookKind::WinLoss, 0, "wins/losses: only real rows"},
    {0x800B5628, HookKind::HomeScene, 0, "home stage: Heihachi's scene in records 18/34"},
    {0x80032E44, HookKind::ForearmMount, 25, "gauntlets: on his forearms, as on the PS2"},
    {0x8015196C, HookKind::CellPlace, 3, "select: a cell's position (helper)", 0},
    {0x80151C44, HookKind::CellPlace, 29, "select: a tile's position", 80},
    {0x80153244, HookKind::CellPlace, 3, "select: the cursor frame's position", 208},
    {0x801530A4, HookKind::CellMove, 0, "select: the cursor reaches his cell"},
    {0x801521B8, HookKind::CellExit, 28, "select: his tile leaves as column 0"},
}};

// His gauntlets. The skeleton function (800311CC) ends at 80032E44 with its world matrices final, in the
// rows its frame keeps a pointer to at r1 + 16 (row-major 4x4 floats, column vectors; row = joint + 2).
// Weapon mount i is row 23 + i; the per-character switch at 800A0DFC gives him parents (8, 12) at model
// + 0xB54, and no other fighter has them. The PS2 hangs his bands from the forearms (rows 8, 12; x along
// the arm, from the elbow) by one fixed transform per side, read from its RAM in a fight (PCSX2, the
// user's disc; both of its matrix buffers, two poses, agree to 4 decimals): band = forearm * (scale
// 1.0368, a turn about the arm, a move of (0.144, 0.0192, 0) in the forearm's axes), the turn 180 degrees
// on the right and none on the left. 1.0368 is his weapon scale (model +0x3C4 on both machines; +0x3C0
// holds his body scale 0.96) and the move is 0.96 * (0.15, 0.02, 0). All twelve of his weapons land on
// the forearm under it. The GC's own mount is not one rule: in a punch (frame 10419 of reg/punch.txt) its
// right mount is already exactly that band while its left is the hand times a fixed hold, and the hand
// bends at the wrist up to 66 degrees. So the hook never reads the mount: it writes band * hold, where
// hold is the fixed hold his halves were built through (tools/heihachi/build_heihachi.py GRIP: each rigid
// vertex v became R^T (v - t); rows of R, then t). Reading the mount as hand * hold applied the band a
// second time wherever the game had already used the forearm, and threw the band off the arm in punches
// (user, 11-10). Only the forearm is read, final at every one of the skeleton's evaluations, so the band
// cannot flicker (the twist bone TEKUBI is refreshed once a frame, late, and a weld to it did); writing
// the same value at each evaluation is idempotent.
constexpr std::uint32_t kModelParents = 0xB54, kFirstMountRow = 23;
constexpr std::array<std::uint8_t, 2> kHisParents{8, 12};
constexpr std::array<std::uint32_t, 2> kHisForearmRows{8, 12};
constexpr float kHisWeaponScale = 1.0368f, kBandAlong = 0.144f, kBandAcross = 0.0192f;
// The band's turn about the forearm's x: 180 degrees on the right, none on the left -- as a sign, exact,
// rather than a host cos/sin whose low bits differ between libms and would reach guest RAM.
constexpr std::array<float, 2> kHisBandFlip{-1.0f, 1.0f};
// The hold per side (right: 90 degrees about y; left: 80 degrees about z), row-major [R | t].
constexpr float kCos80 = 0.173648178f, kSin80 = 0.984807753f;
constexpr std::array<std::array<float, 16>, 2> kHisHold{{
    {0, 0, -1, 0.0768f, 0, 1, 0, 0.0384f, 1, 0, 0, 0, 0, 0, 0, 1},
    {kCos80, -kSin80, 0, -0.03744f, kSin80, kCos80, 0, 0.0576f, 0, 0, 1, 0, 0, 0, 0, 1},
}};

// His select tile. The grid code (80151BA0 and three copies) places cell c at column c % 5, row c / 5:
// x = kGridX + 64 * col, y = kGridY + 62 * row. Cell 9 is the free 5th slot of row 1, right under Spawn;
// the user wanted him on Spawn's row, opposite him: column -1, left of Mitsurugi. Only cell 9 is ever at
// (480, 144), so each place that stores a cell's position swaps that one for (160, 82).
constexpr float kGridX = 224, kGridY = 82, kGridStepX = 64, kGridStepY = 62;

inline std::uint32_t FloatBits(float f)
{
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// The cursor (80152DB0) moves by cell number: up/down +-5 and left/right +-1, wrapping within the grid
// and the row, and stepping past empty cells. The move is final at 801530A4; this rewrites it for his
// cell at column -1 of row 0, whose far end is `last` (Spawn's cell 4, or 3 without him). Buttons: up
// 0x2000, down 0x1000, left 0x800, right 0x400; `max` is the last cell (the cursor's +2).
inline std::uint32_t CursorMove(std::uint32_t old_cell, std::uint32_t game_cell, std::uint32_t buttons,
                                std::uint32_t last, std::uint32_t max)
{
  const bool up = buttons & 0x2000, down = buttons & 0x1000, left = buttons & 0x800, right = buttons & 0x400;
  if (old_cell == kHeihachiCell)  // from him: his column is column 0's neighbour
  {
    if (down)
      return 5;
    if (up)
      return max / 5 * 5;
    if (left)
      return last;
    if (right)
      return 0;
    return old_cell;
  }
  if (!up && !down && ((old_cell == 0 && left) || (old_cell == last && right)))
    return kHeihachiCell;
  if (game_cell != kHeihachiCell)
    return game_cell;
  if (old_cell == 8 && right)  // row 1 wraps past his old place
    return 5;
  if (old_cell == 5 && left)
    return 8;
  return old_cell;  // nothing else lies in his old column
}

inline const SpawnHook* FindHook(std::uint32_t pc)
{
  for (const SpawnHook& h : kUsHooks)
    if (h.pc == pc)
      return &h;
  return nullptr;
}

inline std::uint32_t Read32(const ReadFn& read, std::uint32_t a)
{
  return (std::uint32_t(read(a)) << 24) | (std::uint32_t(read(a + 1)) << 16) |
         (std::uint32_t(read(a + 2)) << 8) | read(a + 3);
}

inline void Write32(const WriteFn& write, std::uint32_t a, std::uint32_t v)
{
  for (std::uint32_t i = 0; i < 4; ++i)
    write(a + i, static_cast<std::uint8_t>(v >> (24 - 8 * i)));
}

// The game's ID -> code lookup, read from RAM: the index, or -1.
inline int CodeOf(std::uint32_t id, const ReadFn& read)
{
  for (std::uint32_t i = 0; i < kCodeCount; ++i)
  {
    const std::uint32_t v = (std::uint32_t(read(kCodeTable + i * 2)) << 8) | read(kCodeTable + i * 2 + 1);
    if (v == id)
      return static_cast<int>(i);
  }
  return -1;
}

// One counter: +1, held at the cap (the game's own rule), and only in a real row.
inline void Count(std::uint32_t obj, std::uint32_t code, std::uint32_t field, const ReadFn& read,
                  const WriteFn& write)
{
  if (code >= kCodeCount)
    return;
  const std::uint32_t a = obj + code * kStatsRow + field;
  const std::uint32_t v = Read32(read, a) + 1;
  Write32(write, a, v < kStatsCap ? v : kStatsCap);
}

inline void MarkDirty(std::uint32_t obj, const ReadFn& read, const WriteFn& write)
{
  Write32(write, obj + kStatsFlags, Read32(read, obj + kStatsFlags) | 1);
}

using Mat4 = std::array<float, 16>;

inline Mat4 ReadMat(const ReadFn& read, std::uint32_t a)
{
  Mat4 m;
  for (int i = 0; i < 16; ++i)
  {
    const std::uint32_t u = Read32(read, a + i * 4);
    std::memcpy(&m[i], &u, 4);
  }
  return m;
}

inline void WriteMat(const WriteFn& write, std::uint32_t a, const Mat4& m)
{
  for (int i = 0; i < 16; ++i)
  {
    std::uint32_t u;
    std::memcpy(&u, &m[i], 4);
    Write32(write, a + i * 4, u);
  }
}

// Explicit std::fma, not a*b+c: the result reaches guest RAM, and whether a compiler contracts a*b+c
// depends on the host and its flags.
inline Mat4 MulMat(const Mat4& a, const Mat4& b)
{
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k)
        r[i * 4 + j] = std::fma(a[i * 4 + k], b[k * 4 + j], r[i * 4 + j]);
  return r;
}

// At `hook`'s PC, before the guest instruction there runs. True when it acted; `pc` then says where
// the guest goes next (the caller, for the hooks that stand in for a whole function).
inline bool ApplyHook(const SpawnHook& hook, const Installed& installed, std::uint32_t* gpr, std::uint32_t& pc,
                      std::uint32_t lr, const ReadFn& read, const WriteFn& write)
{
  switch (hook.kind)
  {
  case HookKind::CodeToId:
    for (const GuestSlot& g : kGuests)
    {
      if (gpr[3] == g.code && installed.Has(g.id))
      {
        gpr[3] = g.id;
        pc = lr;
        return true;
      }
    }
    return false;
  case HookKind::RecordCodeReg:
  case HookKind::RecordCodeMem:
  {
    if (gpr[3] != 0xffffffffu)
      return false;
    const std::uint32_t id =
        hook.kind == HookKind::RecordCodeReg ? (gpr[hook.reg] & 0xff) : read(gpr[hook.reg]);
    const GuestSlot* const g = FindGuest(id);
    if (!g || !installed.Has(id))
      return false;
    gpr[3] = g->code;
    return true;
  }
  case HookKind::HomeScene:
    if (gpr[0] != kHeihachiId || !installed.Has(kHeihachiId))
      return false;
    gpr[5] = 11;  // group-4 records 7 + 11 and 23 + 11
    pc = 0x800B56A4;
    return true;
  case HookKind::CellPlace:
  {
    if (!installed.Has(kHeihachiId))
      return false;
    const std::uint32_t a = gpr[hook.reg] + hook.off;
    if (Read32(read, a) != FloatBits(kGridX + kGridStepX * 4) ||
        Read32(read, a + 4) != FloatBits(kGridY + kGridStepY * 1))
      return false;
    Write32(write, a, FloatBits(kGridX - kGridStepX));  // column -1
    Write32(write, a + 4, FloatBits(kGridY));           // row 0
    return true;
  }
  case HookKind::CellExit:
    // The exit animates a tile by its column (r0 = cell % 5): column 4 only turns and never reaches
    // state 6, as no stock tile is there. His cell 9 was; he sits left of column 0, so he leaves as it.
    if (!installed.Has(kHeihachiId) || Read32(read, gpr[hook.reg] + 120) != kHeihachiCell)
      return false;
    gpr[0] = 0;
    return true;
  case HookKind::CellMove:
  {
    if (!installed.Has(kHeihachiId))
      return false;
    const std::uint32_t moved = CursorMove(gpr[31], gpr[29], gpr[28], installed.Has(kSpawnId) ? 4 : 3,
                                           (std::uint32_t(read(gpr[26] + 2)) << 8) | read(gpr[26] + 3));
    if (moved == gpr[29])
      return false;
    gpr[29] = moved;
    write(gpr[26] + 4, static_cast<std::uint8_t>(moved >> 8));
    write(gpr[26] + 5, static_cast<std::uint8_t>(moved));
    return true;
  }
  case HookKind::ForearmMount:
  {
    if (!installed.Has(kHeihachiId))
      return false;
    const std::uint32_t model = gpr[hook.reg];
    for (std::uint32_t i = 0; i < 2; ++i)
      if (read(model + kModelParents + i) != kHisParents[i])
        return false;
    const std::uint32_t rows = Read32(read, gpr[1] + 16);
    for (std::uint32_t i = 0; i < 2; ++i)
    {
      const Mat4 fore = ReadMat(read, rows + kHisForearmRows[i] * 64u);
      // The PS2's band: the forearm turned about its x (columns 1 and 2 about column 0) and scaled, moved
      // along its own x and y.
      Mat4 band = fore;
      // Every multiply-add is an explicit std::fma: IEEE fixes its result, so the bytes are the same
      // whether or not the compiler would have contracted a*b+c on this host (the run loop's output is
      // hashed and compared between peers).
      const float flip = kHisBandFlip[i];
      for (int r = 0; r < 3; ++r)
      {
        band[r * 4] = kHisWeaponScale * fore[r * 4];
        band[r * 4 + 1] = kHisWeaponScale * (flip * fore[r * 4 + 1]);
        band[r * 4 + 2] = kHisWeaponScale * (flip * fore[r * 4 + 2]);
        band[r * 4 + 3] = std::fma(kBandAcross, fore[r * 4 + 1], std::fma(kBandAlong, fore[r * 4], fore[r * 4 + 3]));
      }
      WriteMat(write, rows + (kFirstMountRow + i) * 64u, MulMat(band, kHisHold[i]));
    }
    return true;
  }
  case HookKind::RowMissing:
  {
    // r5 = the code searched for (8011D548). Only a guest's: a stock fighter with no row keeps the
    // game's -1.
    bool guest = false;
    for (const GuestSlot& g : kGuests)
      guest = guest || (gpr[5] & 0xff) == g.code;
    if (!guest || gpr[28] == 0 || gpr[29] != gpr[28])
      return false;
    gpr[29] = gpr[28] - 1;
    return true;
  }
  case HookKind::PlayCount:
  {
    if (CodeOf(gpr[5], read) >= 0)
      return false;
    const std::uint32_t obj = gpr[3];
    Write32(write, obj + kStatsCode + gpr[4] * 4, 0xffffffffu);
    MarkDirty(obj, read, write);
    pc = lr;
    return true;
  }
  case HookKind::WinLoss:
  {
    const std::uint32_t obj = gpr[3];
    const std::uint32_t c0 = Read32(read, obj + kStatsCode), c1 = Read32(read, obj + kStatsCode + 4);
    if (c0 < kCodeCount && c1 < kCodeCount)
      return false;
    const std::uint32_t col = Read32(read, obj + kStatsMode) != 8 ? 4 : 0;
    const std::uint32_t winner = gpr[4];
    if (winner == 0xffffffffu)
    {
      Count(obj, c0, 28 + col, read, write);  // a draw: the word after each row's losses
      Count(obj, c1, 28 + col, read, write);
    }
    else
    {
      const std::uint32_t w = winner & 1;
      Count(obj, w ? c1 : c0, 12 + col, read, write);
      Count(obj, w ? c0 : c1, 20 + col, read, write);
    }
    MarkDirty(obj, read, write);
    pc = lr;
    return true;
  }
  }
  return false;
}

// Saves made with him before the hooks: the play-count write for code -1 landed on bytes 10-13 of
// player 2's controller settings (two 14-byte blocks at 0x80405D0C, reset from 0x802A42E4; byte 12 is
// the layout preset index), turning 0a 08 00 00 into 00 01 86 9f -- the counter's 99999 cap. Only
// that exact signature is put back, and only to this DOL's own defaults.
constexpr std::uint32_t kPadDefaults = 0x802A42E4, kP2PadDamage = 0x80405D1A + 10;
constexpr std::uint32_t kPadDamage = 0x0001869f;

inline bool RepairP2Pad(const ReadFn& read, const WriteFn& write)
{
  if (Read32(read, kP2PadDamage) != kPadDamage)
    return false;
  const std::uint32_t good = Read32(read, kPadDefaults + 10);
  if (good != 0x0a080000u)
    return false;
  Write32(write, kP2PadDamage, good);
  return true;
}

}  // namespace RecompGuestTables
