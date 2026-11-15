// Spawn as character 29 (RecompGuestCore.h): which builds of his archive the runtime trusts, which
// host file backs the disc's root.olk, and nothing else. No Dolphin dependencies.

#include "RecompGuestCore.h"
#include "RecompGuestTables.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (0)

namespace
{
using namespace RecompGuestCore;
using namespace RecompGuestTables;

const char* const kRoot = "7ee2f6663e5b2386a2008d20428363763843677e";

std::string Hex(std::size_t n, unsigned char v)
{
  static const char* d = "0123456789abcdef";
  std::string r;
  for (std::size_t i = 0; i < n; ++i)
  {
    r += d[v >> 4];
    r += d[v & 15];
  }
  return r;
}

// One guest's object in guests.json. His ending row ships as -1s.
std::string TablesJson(std::size_t descriptor = 248, std::size_t strings = 51, const std::string& cell = "",
                       std::size_t ending = 8, std::size_t preview = 28, int id = 29, bool with_name = true)
{
  const char* const hexid = id == 29 ? "001d" : "001c";
  return "{\"id\": " + std::to_string(id) +
         ", \"block\": 982, \"roster\": 1, \"voice\": 1, \"costume\": \"010101010000\", \"descriptor\": \"" +
         Hex(descriptor, 0xab) + "\", \"strings\": \"" + Hex(strings, 0x41) + "\", \"cell\": \"" +
         (cell.empty() ? hexid + Hex(34, 0xcd) : cell) + "\", \"ending\": \"" + Hex(ending, 0x33) +
         "\", \"ending_old\": \"" + Hex(8, 0xff) + "\", " +
         (with_name ? "\"name\": \"" + Hex(36, 0x53) + "\", " : std::string()) + "\"preview\": \"" +
         Hex(preview, 0x44) + "\"}";
}

// The pools' shipped bytes are zeros here, as FakeRam holds them.
std::string ManifestText(const char* disc = "GRSEAF", const char* base = kRoot, int format = 2,
                         unsigned long long size = 602681344ull, const std::string& guests = "[" + TablesJson() + "]",
                         std::size_t pool_block = 248)
{
  return "{\"format\": " + std::to_string(format) + ", \"disc\": \"" + disc +
         "\", \"converter\": \"abc\", \"gc_root_sha1\": \"" + base +
         "\", \"sha1\": \"1111111111111111111111111111111111111111\", \"size\": " + std::to_string(size) +
         ", \"guests_sha1\": \"2222222222222222222222222222222222222222\", \"edits\": []" +
         ", \"pool_block\": \"" + Hex(pool_block, 0x11) + "\", \"pool_table\": \"" + Hex(172, 0x22) +
         "\", \"pool_block_old\": \"" + Hex(248, 0) + "\", \"pool_table_old\": \"" + Hex(172, 0) +
         "\", \"guests\": " + guests + "}";
}

void ManifestValidEnables()
{
  Manifest m;
  CHECK(Validate(ManifestText(), kRoot, 602681344ull, &m) == Check::Ok);
  CHECK(m.guests_sha1 == "2222222222222222222222222222222222222222");
  CHECK(m.size == 602681344ull);
}

// The builder records which discs the guests came from; the runtime must not mind the extra object.
void ManifestWithXboxRecordValidates()
{
  Manifest m;
  std::string t = ManifestText();
  t.insert(t.size() - 1, ", \"xbox\": {\"release\": \"US\", \"root_sha1\": \"5e34\", \"xbe_sha1\": \"e5d4\"}");
  CHECK(Validate(t, kRoot, 602681344ull, &m) == Check::Ok);
}

void ManifestWrongDiscRejects()
{
  Manifest m;
  CHECK(Validate(ManifestText("GRSPAF"), kRoot, 602681344ull, &m) == Check::WrongDisc);
}

void ManifestHashMismatchRejects()
{
  // A skin-modded root.olk: the archive was built from a different base.
  Manifest m;
  CHECK(Validate(ManifestText(), "0000000000000000000000000000000000000000", 602681344ull, &m) ==
        Check::BaseMismatch);
}

void MissingArchiveRejects()
{
  Manifest m;
  CHECK(Validate(ManifestText(), kRoot, kNoFile, &m) == Check::NoArchive);
}

void ArchiveSizeMismatchRejects()
{
  // An archive cut short (disk full, interrupted copy) is not trusted.
  Manifest m;
  CHECK(Validate(ManifestText(), kRoot, 1000ull, &m) == Check::SizeMismatch);
}

void GarbledOrFutureManifestRejects()
{
  Manifest m;
  CHECK(Validate("{not json", kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate("", kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(ManifestText("GRSEAF", kRoot, 3), kRoot, 602681344ull, &m) == Check::BadManifest);
}

void ResolveOffReturnsOriginal()
{
  CHECK(Resolve("/g/files/root.olk", false, "/g") == "/g/files/root.olk");
}

void ResolveOnReturnsSpawnArchive()
{
  CHECK(Resolve("/g/files/root.olk", true, "/g") == "/g/guests/root-guests.olk");
}

void ResolveOtherFilesUntouched()
{
  CHECK(Resolve("/g/files/bgm.afs", true, "/g") == "/g/files/bgm.afs");
  CHECK(Resolve("/g/files/sub/root.olk", true, "/g") == "/g/files/sub/root.olk");
  CHECK(Resolve("/g/files/xroot.olk", true, "/g") == "/g/files/xroot.olk");
}

void ResolveUnderAFilesFolder()
{
  // The game root itself sits under a folder called files: only its OWN files/root.olk switches.
  const std::string g = "/srv/p/files/RingOut/game";
  CHECK(Resolve(g + "/files/root.olk", true, g) == g + "/guests/root-guests.olk");
  CHECK(Resolve("/srv/p/files/root.olk", true, g) == "/srv/p/files/root.olk");
}

void ResolveRelativeGameRoot()
{
  // `--game game-sp9` (relative) while the disc opens the absolute path: it must still switch,
  // or the tables say Spawn and the archive does not have him (the select screen hangs).
  const std::string cwd = std::filesystem::current_path().string();
  CHECK(Resolve(cwd + "/game-x/files/root.olk", true, "game-x") ==
        std::filesystem::path(cwd + "/game-x/guests/root-guests.olk").lexically_normal().string());
  CHECK(Resolve("game-x/files/root.olk", true, cwd + "/game-x") ==
        std::filesystem::path(cwd + "/game-x/guests/root-guests.olk").lexically_normal().string());
}

void ResolveNormalisesThePath()
{
  CHECK(Resolve("/g//files/./root.olk", true, "/g/") == "/g/guests/root-guests.olk");
}

void PathsFollowTheGameRoot()
{
  CHECK(ManifestPath("/g") == "/g/guests/guests.json");
  CHECK(ArchivePath("/g") == "/g/guests/root-guests.olk");
  CHECK(RootOlkPath("/g") == "/g/files/root.olk");
}

void ManifestTablesParsed()
{
  Manifest m;
  CHECK(Validate(ManifestText(), kRoot, 602681344ull, &m) == Check::Ok);
  CHECK(m.guests.size() == 1);
  const GuestTables& t = m.guests.at(0);
  CHECK(t.id == 29);
  CHECK(t.block == 982);
  CHECK(t.roster == 1);
  CHECK(t.costume == std::vector<std::uint8_t>({1, 1, 1, 1, 0, 0}));
  CHECK(t.descriptor.size() == 248 && t.descriptor[0] == 0xab);
  CHECK(t.strings.size() == 51 && t.strings[50] == 0x41);
  CHECK(t.cell.size() == 36 && t.cell[1] == 29 && t.cell[35] == 0xcd);
  CHECK(m.pool_block.size() == 248 && m.pool_block[0] == 0x11);
  CHECK(m.pool_table.size() == 172 && m.pool_table[171] == 0x22);
  CHECK(m.pool_block_old.size() == 248 && m.pool_table_old.size() == 172);
  CHECK(t.ending.size() == 8 && t.ending[0] == 0x33);
  CHECK(t.ending_old == std::vector<std::uint8_t>(8, 0xff));
  CHECK(t.voice == 1);
  CHECK(t.name.size() == 36 && t.name[0] == 0x53);
}

void ManifestBadTablesRejects()
{
  Manifest m;
  const auto bad = [&](const std::string& g) {
    return ManifestText("GRSEAF", kRoot, 2, 602681344ull, "[" + g + "]");
  };
  CHECK(Validate(bad(TablesJson(247)), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(bad(TablesJson(248, 57)), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(bad("{}"), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(ManifestText("GRSEAF", kRoot, 2, 602681344ull, "[" + TablesJson() + "]", 247), kRoot,
                 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(bad(TablesJson(248, 51, "", 7)), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(bad(TablesJson(248, 51, "", 8, 27)), kRoot, 602681344ull, &m) == Check::BadManifest);
  // No guests, an unknown ID, the same guest twice, and Spawn without his name.
  CHECK(Validate(ManifestText("GRSEAF", kRoot, 2, 602681344ull, "[]"), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(bad(TablesJson(248, 51, "0016" + Hex(34, 0xcd), 8, 28, 22)), kRoot, 602681344ull, &m) ==
        Check::BadManifest);
  CHECK(Validate(ManifestText("GRSEAF", kRoot, 2, 602681344ull, "[" + TablesJson() + "," + TablesJson() + "]"),
                 kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(bad(TablesJson(248, 51, "", 8, 28, 29, false)), kRoot, 602681344ull, &m) == Check::BadManifest);
}

// A manifest from before the guest list says what to do about it, and turns guests off.
void OldManifestAsksForARebuild()
{
  Manifest m;
  CHECK(Validate(ManifestText("GRSEAF", kRoot, 1), kRoot, 602681344ull, &m) == Check::OldFormat);
  CHECK(std::string(Describe(Check::OldFormat)).find("rebuild guests") != std::string::npos);
}

// Two manifests with different guest sets must not look alike to netplay.
void GuestsHashIsTheManifests()
{
  Manifest a, b;
  std::string other = ManifestText();
  other.replace(other.find("2222"), 4, "3333");
  CHECK(Validate(ManifestText(), kRoot, 602681344ull, &a) == Check::Ok);
  CHECK(Validate(other, kRoot, 602681344ull, &b) == Check::Ok);
  CHECK(!a.guests_sha1.empty() && a.guests_sha1 != b.guests_sha1);
}

// Guest RAM stand-in: MEM1 from 0x80000000, the DOL's tables as shipped.
struct FakeRam
{
  std::vector<std::uint8_t> b = std::vector<std::uint8_t>(0x500000, 0);  // up to 0x80500000: the stats object and pad settings
  int writes = 0;
  FakeRam()
  {
    using namespace RecompGuestTables;
    b[kBlockAddr - 0x80000000 + 29 * 2] = 0xff;
    b[kBlockAddr - 0x80000000 + 29 * 2 + 1] = 0xff;
    b[kRosterAddr - 0x80000000 + 29] = 0x13;
    b[kLinkIdField - 0x80000000 + 1] = 27;  // the DOL is loaded
    b[kCellAddr - 0x80000000 + 4 * kCellStride] = 0xff;  // cell 4: the -1 end marker
    for (int i = 0; i < 8; ++i)
      b[kEndingAddr - 0x80000000 + 29 * kEndingStride + i] = 0xff;  // his ending row: -1s
    b[kCellAddr - 0x80000000 + 4 * kCellStride + 1] = 0xff;
    const std::uint8_t takagi[4] = {0x80, 0x28, 0xed, 0x7c};  // name pointer 29: the dummy's Q.TAKAGI
    for (int i = 0; i < 4; ++i)
      b[kNameTableAddr - 0x80000000 + 29 * 4 + i] = takagi[i];
  }
  ReadFn Read()
  {
    return [this](std::uint32_t a) { return b.at(a - 0x80000000); };
  }
  WriteFn Write()
  {
    return [this](std::uint32_t a, std::uint8_t v) {
      b.at(a - 0x80000000) = v;
      ++writes;
    };
  }
};

std::vector<SpawnPatch> Patches()
{
  Manifest m;
  Validate(ManifestText(), kRoot, 602681344ull, &m);
  return MakeUsPatches(m);
}

void PatchesCoverEveryTable()
{
  using namespace RecompGuestTables;
  const auto p = Patches();
  CHECK(p.size() == 14);
  CHECK(p[2].addr == kBlockAddr + 58 && p[2].bytes == std::vector<std::uint8_t>({0x03, 0xd6}));
  // ID -> select cell: his own cell (4), not the Xbox's 1, which is Link's cell on GC. Every ID -> cell
  // lookup (cursor placement, cell sprites, the ranking's face at 80158688) then finds his record.
  CHECK(p[3].addr == kRosterAddr + 29 && p[3].bytes == std::vector<std::uint8_t>({kSpawnCell}));
  CHECK(p[4].addr == kCostumeAddr + 29 * 6 && p[4].bytes.size() == 6);
  CHECK(p[5].addr == kStringsAddr && p[5].bytes.size() == kStringsSize);  // padded with zeros
  CHECK(p[6].addr == kDescAddr + 29 * kDescStride && p[6].bytes.size() == kDescStride);
  // His select-screen cell: the free 5th slot of Link's row (cell 4), which holds the -1 end marker.
  CHECK(p[7].addr == kCellAddr + 4 * kCellStride && p[7].bytes.size() == kCellStride);
  CHECK(p[7].expect.size() == kCellStride && p[7].expect[0] == 0xff && p[7].expect[1] == 0xff &&
        p[7].expect[2] == 0);
  // The character pools, repacked with him beside Link, and the pointer table that reaches them: the
  // shipped bytes they replace come from the player's own DOL, through the manifest.
  CHECK(p[0].addr == kPoolAddr && p[0].bytes.size() == kPoolSize && p[0].bytes[0] == 0x11 &&
        p[0].expect == std::vector<std::uint8_t>(kPoolSize, 0));
  CHECK(p[1].addr == kPoolTableAddr && p[1].bytes.size() == kPoolTableSize && p[1].bytes[0] == 0x22 &&
        p[1].expect == std::vector<std::uint8_t>(kPoolTableSize, 0));
  // His ending row (illustration, pages, text): GC ships -1s, and the ending then sizes entry -1 and
  // places its next buffer past the end of RAM.
  CHECK(p[8].addr == kEndingAddr + 29 * kEndingStride && p[8].bytes.size() == kEndingStride &&
        p[8].bytes[0] == 0x33 && p[8].expect == std::vector<std::uint8_t>(kEndingStride, 0xff));
  // His English-voice flag: GC ships 0 (the dummy), so he spoke Japanese in an English game.
  CHECK(p[9].addr == kVoiceAddr + 29 && p[9].expect == std::vector<std::uint8_t>({0}) &&
        p[9].bytes == std::vector<std::uint8_t>({1}));
  // His name: his Xbox string in empty descriptor row 8, and pointer 29 moved from Q.TAKAGI
  // (0x8028ED7C) to it. The strings of 28 and 29 stay as shipped.
  CHECK(p[10].addr == kNameAt && p[10].bytes.size() == kNameRoom && p[10].bytes[0] == 0x53 &&
        p[10].expect == std::vector<std::uint8_t>(kNameRoom, 0));
  CHECK(p[11].addr == kNameTableAddr + 29 * 4 &&
        p[11].expect == std::vector<std::uint8_t>({0x80, 0x28, 0xed, 0x7c}) &&
        p[11].bytes == std::vector<std::uint8_t>({0x80, 0x27, 0x62, 0xe0}));
  // His weapon-select preview pose: his Xbox bone record in empty descriptor row 7, and row 29 of the
  // pose table (GC ships count 0, which left the preview parked off screen) pointing at it.
  CHECK(p[12].addr == kPreviewHome && p[12].bytes.size() == 28 && p[12].bytes[0] == 0x44 &&
        p[12].expect == std::vector<std::uint8_t>(28, 0));
  CHECK(p[13].addr == kPreviewTable + 29 * 8 && p[13].expect == std::vector<std::uint8_t>(8, 0) &&
        p[13].bytes == std::vector<std::uint8_t>({0, 0, 0, 1, 0x80, 0x27, 0x61, 0x8c}));
}

// Nothing of his may touch ID 28's descriptor or the shipped name strings: both are Heihachi's.
void MovedSpawnData()
{
  using namespace RecompGuestTables;
  const std::uint32_t row7 = kDescAddr + 7 * kDescStride, row8 = kDescAddr + 8 * kDescStride;
  CHECK(kStringsAddr == row7 + 0xC0 && kPreviewHome == row7 + 0x64 && kNameAt == row8 + 0xC0);
  const std::uint32_t row28 = kDescAddr + 28 * kDescStride;
  for (const auto& p : Patches())
  {
    const std::uint32_t end = p.addr + static_cast<std::uint32_t>(p.bytes.size());
    CHECK(end <= row28 || p.addr >= row28 + kDescStride);
    CHECK(end <= 0x8028ED6A || p.addr >= 0x8028ED6A + 36);
  }
}

// Pools that differ from what the manifest says this DOL ships: no writes at all.
void PoolsMismatchWritesNothing()
{
  FakeRam ram;
  ram.b[RecompGuestTables::kPoolAddr - 0x80000000 + 3] = 9;
  const auto r = ApplyPatches(Patches(), ram.Read(), ram.Write());
  CHECK(r.result == PatchResult::Mismatch);
  CHECK(std::string(r.what) == "character pools");
  CHECK(ram.writes == 0);
}

// The cell record must name him: a cell for any other ID would put a second copy of that fighter
// on the grid.
void ManifestCellMustBeSpawn()
{
  Manifest m;
  const auto with = [](const std::string& cell) {
    return ManifestText("GRSEAF", kRoot, 2, 602681344ull, "[" + TablesJson(248, 51, cell) + "]");
  };
  CHECK(Validate(with("001b" + Hex(34, 0xcd)), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(with("001c" + Hex(34, 0xcd)), kRoot, 602681344ull, &m) == Check::BadManifest);
  CHECK(Validate(with("001d" + Hex(30, 0xcd)), kRoot, 602681344ull, &m) == Check::BadManifest);
}

void PatchesWriteWhenExpected()
{
  FakeRam ram;
  const auto p = Patches();
  CHECK(ApplyPatches(p, ram.Read(), ram.Write()).result == PatchResult::Applied);
  for (const auto& patch : p)
    for (std::size_t i = 0; i < patch.bytes.size(); ++i)
      CHECK(ram.b[patch.addr - 0x80000000 + i] == patch.bytes[i]);
  // A second pass (or a savestate taken after the first) finds them in place and writes nothing.
  const int before = ram.writes;
  CHECK(ApplyPatches(p, ram.Read(), ram.Write()).result == PatchResult::AlreadyApplied);
  CHECK(ram.writes == before);
}

void PatchesAllOrNothing()
{
  FakeRam ram;
  ram.b[RecompGuestTables::kRosterAddr - 0x80000000 + 29] = 0x07;  // another build of the game
  const auto r = ApplyPatches(Patches(), ram.Read(), ram.Write());
  CHECK(r.result == PatchResult::Mismatch);
  CHECK(std::string(r.what) == "roster slot");
  CHECK(ram.writes == 0);
}

void PatchesWaitForTheDol()
{
  FakeRam ram;
  ram.b[RecompGuestTables::kLinkIdField - 0x80000000 + 1] = 0;  // apploader still running
  CHECK(ApplyPatches(Patches(), ram.Read(), ram.Write()).result == PatchResult::NotReady);
  CHECK(ram.writes == 0);
}

void OffWritesNothing()
{
  FakeRam ram;
  BootPatcher patcher;
  for (int frame = 0; frame < 3; ++frame)
    patcher.OnFrame(false, Patches(), ram.Read(), ram.Write());
  CHECK(ram.writes == 0);
  patcher.OnFrame(true, Patches(), ram.Read(), ram.Write());
  CHECK(ram.writes > 0);
  CHECK(patcher.Done());
}
}  // namespace

// Task 7: per-character code sites. At a gated site the ID register is read by a compare; while it
// holds 29 the hook swaps in the ID whose case Spawn takes, for that one decision only.
static void SitesAreTheClassifiedOnes()
{
  using namespace RecompGuestTables;
  CHECK(IsSitePc(0x800834DC) && IsSitePc(0x80083594) && IsSitePc(0x800B5628));
  CHECK(FindSite(0x800834DC, 29) && FindSite(0x800834DC, 29)->as_id == 24);  // Xbox groups him with 24/25
  CHECK(FindSite(0x80083594, 29) && FindSite(0x80083594, 29)->as_id == 24);
  // His own home-stage scene is converted into the records the switch's default reaches (7/23), so
  // this site keeps him as 29 rather than borrowing Talim's scene (user report 2026-11-04).
  CHECK(FindSite(0x800B5628, 29) && FindSite(0x800B5628, 29)->as_id == 29);
  CHECK(!IsSitePc(0x800834E0) && FindSite(0x800834E0, 29) == nullptr);
  CHECK(FindSite(0x800834DC, 22) == nullptr);
}

static void SiteSwapsOnlyAnInstalledGuest()
{
  using namespace RecompGuestTables;
  const SpawnSite s{0x800834DC, 29, 24, "t"};
  const Installed spawn{true, false}, heihachi{false, true};
  std::uint32_t gpr[32] = {};
  const std::uint32_t cmplwi_r0_27 = (10u << 26) | (0u << 16) | 27u;
  gpr[0] = 29;
  CHECK(!ApplySite(s, heihachi, cmplwi_r0_27, gpr) && gpr[0] == 29);
  CHECK(ApplySite(s, spawn, cmplwi_r0_27, gpr) && gpr[0] == 24);
  gpr[0] = 22;
  CHECK(!ApplySite(s, spawn, cmplwi_r0_27, gpr) && gpr[0] == 22);
  const std::uint32_t cmpwi_r5_27 = (11u << 26) | (5u << 16) | 27u;
  gpr[5] = 29;
  CHECK(ApplySite(s, spawn, cmpwi_r5_27, gpr) && gpr[5] == 24);
}

static void SiteIgnoresAnUnexpectedInstruction()
{
  using namespace RecompGuestTables;
  const SpawnSite s{0x800834DC, 29, 24, "t"};
  std::uint32_t gpr[32] = {};
  gpr[0] = 29;
  CHECK(!ApplySite(s, Installed{true, false}, 0x60000000u, gpr) && gpr[0] == 29);  // a nop: a different DOL
}


// Character codes: rankings and per-character stats file a fighter under a code 0-23, from the
// 24-entry ID table at 0x80274D10. 29 is not in it, so the lookup (800914C4) gives -1: his ranking
// records showed another fighter's face, and the stats writers indexed their 28-byte rows with -1
// and overwrote the bytes in front of the table (seen in RAM: ...09 0b 0a 08 -> ...09 0b 00 01 86 9f).
namespace
{
const RecompGuestTables::Installed kOnlySpawn{true, false};
const std::uint16_t kGcCodes[24] = {21, 22, 26, 23, 1, 3, 5, 17, 18, 11, 12, 13,
                                    4,  25, 27, 6,  2, 15, 24, 20, 19, 30, 31, 32};

void PutCodes(FakeRam& ram)
{
  for (int i = 0; i < 24; ++i)
  {
    ram.b[kCodeTable - 0x80000000 + i * 2] = 0;
    ram.b[kCodeTable - 0x80000000 + i * 2 + 1] = static_cast<std::uint8_t>(kGcCodes[i]);
  }
}

std::uint32_t Rd32(FakeRam& ram, std::uint32_t a)
{
  const std::size_t o = a - 0x80000000;
  return (std::uint32_t(ram.b[o]) << 24) | (std::uint32_t(ram.b[o + 1]) << 16) |
         (std::uint32_t(ram.b[o + 2]) << 8) | ram.b[o + 3];
}

void Wr32(FakeRam& ram, std::uint32_t a, std::uint32_t v)
{
  const std::size_t o = a - 0x80000000;
  for (int i = 0; i < 4; ++i)
    ram.b[o + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

constexpr std::uint32_t kObj = 0x80105D38;  // a stats object in FakeRam's range
constexpr std::uint32_t kLr = 0x80084824;

struct Call
{
  std::uint32_t gpr[32] = {};
  std::uint32_t pc = 0;
};

bool Run(FakeRam& ram, std::uint32_t pc, Call& c)
{
  c.pc = pc;
  const SpawnHook* h = FindHook(pc);
  return h && ApplyHook(*h, kOnlySpawn, c.gpr, c.pc, kLr, ram.Read(), ram.Write());
}
}  // namespace

static void AFighterWithNoRowTakesTheLastOne()
{
  FakeRam ram;
  Call c;  // the search ran off the end: r29 (row found) == r28 (rows searched); r5 = his code
  c.gpr[5] = kSpawnCode;
  c.gpr[28] = 20;
  c.gpr[29] = 20;
  CHECK(Run(ram, 0x8011D574, c) && c.gpr[29] == 19 && c.pc == 0x8011D574);
  Call d;  // found: the game's own row, untouched
  d.gpr[5] = kSpawnCode;
  d.gpr[28] = 20;
  d.gpr[29] = 7;
  CHECK(!Run(ram, 0x8011D574, d) && d.gpr[29] == 7);
  Call e;  // a board with no rows: nothing to take, the game's -1 stands
  e.gpr[5] = kSpawnCode;
  CHECK(!Run(ram, 0x8011D574, e) && e.gpr[29] == 0);
  Call f;  // a smaller board (locked fighters): its own last row
  f.gpr[5] = kSpawnCode;
  f.gpr[28] = 13;
  f.gpr[29] = 13;
  CHECK(Run(ram, 0x8011D574, f) && f.gpr[29] == 12);
  Call g;  // a STOCK fighter with no row (code 5): the game's -1 stands, nobody's row is taken
  g.gpr[5] = 5;
  g.gpr[28] = 20;
  g.gpr[29] = 20;
  CHECK(!Run(ram, 0x8011D574, g) && g.gpr[29] == 20);
  Call h;  // the second guest (Heihachi, code 25) is a guest too
  h.gpr[5] = kGuests[1].code;
  h.gpr[28] = 20;
  h.gpr[29] = 20;
  CHECK(Run(ram, 0x8011D574, h) && h.gpr[29] == 19);
}

// Spawn against Heihachi, and a Heihachi mirror: every site and hook acts on the ID in its own register.
static void TwoGuests()
{
  const Installed both{true, true}, spawn{true, false}, heihachi{false, true};
  const std::uint32_t cmplwi_r0_27 = (10u << 26) | 27u;
  CHECK(FindSite(0x800834DC, 28) && FindSite(0x800834DC, 28)->as_id == 24);
  CHECK(FindSite(0x80083594, 28) && FindSite(0x80083594, 28)->as_id == 24);
  CHECK(FindSite(0x800B5628, 28) == nullptr);  // a hook: no GC case reaches his scene
  std::uint32_t gpr[32] = {};
  gpr[0] = 28;  // Heihachi in this decision, whoever else is fighting
  CHECK(ApplySite(*FindSite(0x800834DC, 28), both, cmplwi_r0_27, gpr) && gpr[0] == 24);
  gpr[0] = 29;
  CHECK(ApplySite(*FindSite(0x800834DC, 29), both, cmplwi_r0_27, gpr) && gpr[0] == 24);
  gpr[0] = 28;  // a mirror behaves as a single guest; an absent guest is left alone
  CHECK(ApplySite(*FindSite(0x80083594, 28), heihachi, cmplwi_r0_27, gpr) && gpr[0] == 24);
  gpr[0] = 28;
  CHECK(!ApplySite(*FindSite(0x80083594, 28), spawn, cmplwi_r0_27, gpr) && gpr[0] == 28);

  const SpawnHook* const scene = FindHook(0x800B5628);
  CHECK(scene && scene->kind == HookKind::HomeScene);
  const ReadFn no_read = [](std::uint32_t) -> std::uint8_t { return 0; };
  const WriteFn no_write = [](std::uint32_t, std::uint8_t) {};
  std::uint32_t pc = 0x800B5628;
  gpr[0] = 28;
  gpr[5] = 0;
  CHECK(ApplyHook(*scene, both, gpr, pc, 0, no_read, no_write) && gpr[5] == 11 && pc == 0x800B56A4);
  for (const std::uint32_t other : {29u, 27u, 3u})
  {
    pc = 0x800B5628;
    gpr[0] = other;
    gpr[5] = 0;
    CHECK(!ApplyHook(*scene, both, gpr, pc, 0, no_read, no_write) && gpr[5] == 0 && pc == 0x800B5628);
  }
  gpr[0] = 28;
  CHECK(!ApplyHook(*scene, spawn, gpr, pc, 0, no_read, no_write) && pc == 0x800B5628);

  // Both in one manifest: his roster row, cell 9 over the end marker, no name fields.
  Manifest m;
  const std::string guests = "[" + TablesJson() + ", " + TablesJson(248, 74, "", 8, 56, 28, false) + "]";
  CHECK(Validate(ManifestText("GRSEAF", kRoot, 2, 602681344ull, guests), kRoot, 602681344ull, &m) == Check::Ok);
  const auto patches = MakeUsPatches(m);
  int roster = 0, cell = 0;
  for (const auto& p : patches)
  {
    if (p.addr == kRosterAddr + 28)
      roster += p.bytes == std::vector<std::uint8_t>({9}) && p.expect == std::vector<std::uint8_t>({0x13});
    if (p.addr == kCellAddr + 9 * kCellStride)
      cell += p.expect == CellEndMarker();
    CHECK(p.addr != kNameTableAddr + 28 * 4);
  }
  CHECK(roster == 1 && cell == 1);
}

static void HooksAreTheClassifiedOnes()
{
  CHECK(kCodeTable == 0x80274D10u && kCodeCount == 24 && kSpawnCode == 24);
  CHECK(kUsHooks.size() == 13);
  CHECK(FindHook(0x801521B8) && FindHook(0x801521B8)->kind == HookKind::CellExit);
  for (const std::uint32_t pc : {0x8015196Cu, 0x80151C44u, 0x80153244u})
    CHECK(FindHook(pc) && FindHook(pc)->kind == HookKind::CellPlace);
  CHECK(FindHook(0x801530A4) && FindHook(0x801530A4)->kind == HookKind::CellMove);
  CHECK(FindHook(0x80032E44) && FindHook(0x80032E44)->kind == HookKind::ForearmMount);
  CHECK(FindHook(0x8011D574) && FindHook(0x8011D574)->kind == HookKind::RowMissing);
  CHECK(FindHook(0x80091494) && FindHook(0x80091494)->kind == HookKind::CodeToId);
  CHECK(FindHook(0x8011D444) && FindHook(0x8011D444)->kind == HookKind::RecordCodeReg);
  CHECK(FindHook(0x8011DBE8) && FindHook(0x8011DBE8)->kind == HookKind::RecordCodeMem);
  CHECK(FindHook(0x80178BD4) && FindHook(0x80178BD4)->kind == HookKind::PlayCount);
  CHECK(FindHook(0x80178C50) && FindHook(0x80178C50)->kind == HookKind::WinLoss);
  CHECK(FindHook(0x800834DC) == nullptr);  // a compare site, not a hook
}

static void CodeLookupReadsTheTable()
{
  FakeRam ram;
  PutCodes(ram);
  CHECK(CodeOf(21, ram.Read()) == 0);
  CHECK(CodeOf(22, ram.Read()) == 1);
  CHECK(CodeOf(32, ram.Read()) == 23);
  CHECK(CodeOf(29, ram.Read()) == -1);
}

static void HisCodeReadsBackAsHim()
{
  FakeRam ram;
  Call c;
  c.gpr[3] = kSpawnCode;
  CHECK(Run(ram, 0x80091494, c) && c.gpr[3] == 29 && c.pc == kLr);  // returns to the caller
  Call d;
  d.gpr[3] = 1;
  CHECK(!Run(ram, 0x80091494, d) && d.gpr[3] == 1 && d.pc == 0x80091494);  // Talim stays Talim
  Call e;
  e.gpr[3] = 0xff;  // an empty team slot: the game's own clamp handles it
  CHECK(!Run(ram, 0x80091494, e) && e.gpr[3] == 0xff);
  CHECK(ram.writes == 0);
}

static void RecordGetsHisCodeOnlyForHim()
{
  FakeRam ram;
  Call c;
  c.gpr[3] = 0xffffffffu;  // the lookup missed
  c.gpr[19] = 0x1d;        // ...for 29
  CHECK(Run(ram, 0x8011D444, c) && c.gpr[3] == kSpawnCode && c.pc == 0x8011D444);
  Call d;
  d.gpr[3] = 0xffffffffu;
  d.gpr[19] = 0;  // random, or anything else the table lacks: left as the game made it
  CHECK(!Run(ram, 0x8011D444, d) && d.gpr[3] == 0xffffffffu);
  Call e;
  e.gpr[3] = 1;
  e.gpr[19] = 22;
  CHECK(!Run(ram, 0x8011D444, e) && e.gpr[3] == 1);
  Call f;  // team member: the ID is the byte r19 points at
  ram.b[0x100] = 29;
  f.gpr[3] = 0xffffffffu;
  f.gpr[19] = 0x80000100;
  CHECK(Run(ram, 0x8011DBE8, f) && f.gpr[3] == kSpawnCode);
  ram.b[0x100] = 0xff;  // an empty member
  Call g;
  g.gpr[3] = 0xffffffffu;
  g.gpr[19] = 0x80000100;
  CHECK(!Run(ram, 0x8011DBE8, g) && g.gpr[3] == 0xffffffffu);
  CHECK(ram.writes == 0);
}

static void PlayCountWritesNoRowForHim()
{
  FakeRam ram;
  PutCodes(ram);
  Wr32(ram, kObj - 20, 0x0a080000);  // the word -1's row would have counted in
  Wr32(ram, kObj + 0x2C8, 0x10);
  Call c;
  c.gpr[3] = kObj;
  c.gpr[4] = 1;  // player 2
  c.gpr[5] = 29;
  const int before = ram.writes;
  CHECK(Run(ram, 0x80178BD4, c) && c.pc == kLr);
  CHECK(Rd32(ram, kObj - 20) == 0x0a080000);           // untouched
  CHECK(Rd32(ram, kObj + 0x2B8 + 4) == 0xffffffffu);   // his slot's code, as the game stores it
  CHECK(Rd32(ram, kObj + 0x2C8) == 0x11);              // the dirty flag
  CHECK(ram.writes - before == 8);
  Call d;  // anyone with a code runs the game's own function
  d.gpr[3] = kObj;
  d.gpr[5] = 22;
  const int mid = ram.writes;
  CHECK(!Run(ram, 0x80178BD4, d) && d.pc == 0x80178BD4 && ram.writes == mid);
}

static void WinLossCountsOnlyRealRows()
{
  FakeRam ram;
  const std::uint32_t talim = 1, cap = 99999;
  Wr32(ram, kObj + 0x2B0, 8);                  // mode word: 8 -> column 0
  Wr32(ram, kObj + 0x2B8, talim);              // P1 Talim
  Wr32(ram, kObj + 0x2BC, 0xffffffffu);        // P2 Spawn
  Wr32(ram, kObj - 28 + 20, 0x0a080000);       // where -1's losses would land
  Wr32(ram, kObj + talim * 28 + 12, 5);        // Talim's wins
  Call c;
  c.gpr[3] = kObj;
  c.gpr[4] = 0;  // P1 won
  CHECK(Run(ram, 0x80178C50, c) && c.pc == kLr);
  CHECK(Rd32(ram, kObj + talim * 28 + 12) == 6);
  CHECK(Rd32(ram, kObj - 28 + 20) == 0x0a080000);
  CHECK((Rd32(ram, kObj + 0x2C8) & 1) == 1);
  // Spawn won (slot 1), column 1 (mode != 8): Talim's loss counts, his win does not.
  Wr32(ram, kObj + 0x2B0, 3);
  Wr32(ram, kObj + talim * 28 + 20 + 4, cap);  // already at the cap
  Wr32(ram, kObj - 28 + 12 + 4, 0x0a080000);
  Call d;
  d.gpr[3] = kObj;
  d.gpr[4] = 1;
  CHECK(Run(ram, 0x80178C50, d));
  CHECK(Rd32(ram, kObj + talim * 28 + 20 + 4) == cap);
  CHECK(Rd32(ram, kObj - 28 + 12 + 4) == 0x0a080000);
  // A draw (-1): Talim's draw counts (row + 28), his does not.
  Wr32(ram, kObj + talim * 28 + 28 + 4, 2);
  Wr32(ram, kObj + 0 + 4, 0x0a080000);  // -1's draw word: row 0's first word
  Call e;
  e.gpr[3] = kObj;
  e.gpr[4] = 0xffffffffu;
  CHECK(Run(ram, 0x80178C50, e));
  CHECK(Rd32(ram, kObj + talim * 28 + 28 + 4) == 3);
  CHECK(Rd32(ram, kObj + 4) == 0x0a080000);
  // Two real codes: the game's own function.
  Wr32(ram, kObj + 0x2BC, 5);
  Call f;
  f.gpr[3] = kObj;
  const int before = ram.writes;
  CHECK(!Run(ram, 0x80178C50, f) && ram.writes == before);
}

// Saves made with him before the hooks carry the play-count write: player 2's controller settings
// (14 bytes at 0x80405D1A, defaults at 0x802A42E4) end in 00 01 86 9f instead of 0a 08 00 00, and
// byte 12 is the layout preset index. Exactly that signature gets the defaults' bytes back.
static void RepairsOnlyTheKnownDamage()
{
  FakeRam ram;
  const std::uint8_t defaults[14] = {0, 4, 0x0b, 3, 1, 4, 2, 4, 9, 0x0b, 0x0a, 8, 0, 0};
  for (int i = 0; i < 14; ++i)
    ram.b[kPadDefaults - 0x80000000 + i] = defaults[i];
  Wr32(ram, kP2PadDamage, 0x0001869f);
  CHECK(RepairP2Pad(ram.Read(), ram.Write()));
  CHECK(Rd32(ram, kP2PadDamage) == 0x0a080000);
  const int after = ram.writes;
  CHECK(!RepairP2Pad(ram.Read(), ram.Write()) && ram.writes == after);  // nothing left to do
  Wr32(ram, kP2PadDamage, 0x0a080300);  // a player's own preset: not ours to touch
  CHECK(!RepairP2Pad(ram.Read(), ram.Write()) && Rd32(ram, kP2PadDamage) == 0x0a080300);
  ram.b[kPadDefaults - 0x80000000 + 10] = 0x55;  // another build's defaults: write nothing
  Wr32(ram, kP2PadDamage, 0x0001869f);
  CHECK(!RepairP2Pad(ram.Read(), ram.Write()) && Rd32(ram, kP2PadDamage) == 0x0001869f);
}

// A saved row with an absent guest's code stays an ordinary out-of-range code: the game's own clamp
// shows another fighter, and code -> ID never returns a fighter the archive does not hold.
static void CodeOfAnAbsentGuestPassesThrough()
{
  using namespace RecompGuestTables;
  const SpawnHook& h = *FindHook(0x80091494);
  FakeRam ram;
  std::uint32_t gpr[32] = {};
  std::uint32_t pc = 0x80091494;
  gpr[3] = 25;
  CHECK(!ApplyHook(h, Installed{true, false}, gpr, pc, 0x80001000, ram.Read(), ram.Write()) && gpr[3] == 25);
  gpr[3] = 24;
  CHECK(!ApplyHook(h, Installed{false, true}, gpr, pc, 0x80001000, ram.Read(), ram.Write()) && gpr[3] == 24);
  gpr[3] = 24;
  CHECK(ApplyHook(h, Installed{true, true}, gpr, pc, 0x80001000, ram.Read(), ram.Write()) && gpr[3] == 29);
  gpr[3] = 25;
  CHECK(ApplyHook(h, Installed{true, true}, gpr, pc, 0x80001000, ram.Read(), ram.Write()) && gpr[3] == 28);
}

// His gauntlets: the game hangs them from his hands, which bend at the wrist up to 66 degrees in his
// poses; the hook re-hangs them from his forearms by the PS2's own transform.
namespace
{
using Mat = std::array<float, 16>;  // row-major, column vectors: translation in [3], [7], [11]
Mat Mul(const Mat& a, const Mat& b)
{
  Mat r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k)
        r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
  return r;
}
Mat Turn(float deg, float x, float y, float z, int axis)  // a turn about x, y or z, then a move
{
  const float c = std::cos(deg * 3.14159265f / 180), n = std::sin(deg * 3.14159265f / 180);
  Mat m{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
  const int a = (axis + 1) % 3, b = (axis + 2) % 3;
  m[a * 4 + a] = c, m[a * 4 + b] = -n, m[b * 4 + a] = n, m[b * 4 + b] = c;
  return m;
}
struct Ram
{
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x80000, 0);
  ReadFn Read() { return [this](std::uint32_t a) -> std::uint8_t { return bytes[a - 0x80300000]; }; }
  WriteFn Write() { return [this](std::uint32_t a, std::uint8_t v) { bytes[a - 0x80300000] = v; }; }
  void Put(std::uint32_t a, const Mat& m)
  {
    for (int i = 0; i < 16; ++i)
    {
      std::uint32_t u;
      std::memcpy(&u, &m[i], 4);
      Write32(Write(), a + i * 4, u);
    }
  }
  Mat Get(std::uint32_t a)
  {
    Mat m;
    for (int i = 0; i < 16; ++i)
    {
      const std::uint32_t u = Read32(Read(), a + i * 4);
      std::memcpy(&m[i], &u, 4);
    }
    return m;
  }
};
bool Near(const Mat& a, const Mat& b)
{
  for (int i = 0; i < 16; ++i)
    if (std::fabs(a[i] - b[i]) > 1e-5f)
      return false;
  return true;
}
}  // namespace

static void GauntletsHangAsOnThePs2()
{
  const std::uint32_t obj = 0x80350510, sp = 0x80370000, rows = 0x80362560;  // rows: the world matrices
  const SpawnHook* const hook = FindHook(0x80032E44);
  CHECK(hook != nullptr);
  if (!hook)
    return;
  const Installed both{true, true}, spawn{true, false};
  // Per side: the forearm (rows 8, 12; x along the arm), TEKUBI on it turned about the arm by however
  // far the move twists the wrist, the hand bent off TEKUBI, the mount at a grip.
  const Mat fore_r = Turn(30, 1, 2, 3, 2), fore_l = Turn(-70, -1, 2, 3, 1);
  const Mat bend_r = Turn(40, 0.0288f, 0, 0, 2), bend_l = Turn(-66, 0.0288f, 0, 0, 2);
  Mat grip_r = Turn(90, 0.0768f, 0.0384f, 0, 1), grip_l = Turn(80, -0.03744f, 0.0576f, 0, 2);
  for (int i = 0; i < 12; ++i)
    if (i % 4 != 3)
      grip_r[i] *= 1.0368f, grip_l[i] *= 1.0368f;
  struct Pose
  {
    Mat tek_r, tek_l, te_r, te_l, mid_r, mid_l;
  };
  // The mid-forearm twist bones (rows 155, 153) turn about the arm by part of the wrist's twist.
  const auto pose = [&](float twist_r, float twist_l, float mid_tw_r, float mid_tw_l) {
    Pose p;
    p.mid_r = Mul(fore_r, Turn(mid_tw_r, 0.11f, 0, 0, 0)), p.mid_l = Mul(fore_l, Turn(mid_tw_l, 0.11f, 0, 0, 0));
    p.tek_r = Mul(fore_r, Turn(twist_r, 0.22f, 0, 0, 0)), p.tek_l = Mul(fore_l, Turn(twist_l, 0.22f, 0, 0, 0));
    p.te_r = Mul(p.tek_r, bend_r), p.te_l = Mul(p.tek_l, bend_l);
    return p;
  };
  const auto setup = [&](Ram& ram, std::uint8_t right, std::uint8_t left, const Pose& p) {
    ram.bytes[obj + 0xB54 - 0x80300000] = right;
    ram.bytes[obj + 0xB55 - 0x80300000] = left;
    Write32(ram.Write(), sp + 16, rows);
    ram.Put(rows + 8 * 64, fore_r);
    ram.Put(rows + 12 * 64, fore_l);
    ram.Put(rows + 9 * 64, p.te_r);
    ram.Put(rows + 13 * 64, p.te_l);
    ram.Put(rows + 156 * 64, p.tek_r);
    ram.Put(rows + 154 * 64, p.tek_l);
    ram.Put(rows + 155 * 64, p.mid_r);
    ram.Put(rows + 153 * 64, p.mid_l);
    ram.Put(rows + 23 * 64, Mul(p.te_r, grip_r));
    ram.Put(rows + 24 * 64, Mul(p.te_l, grip_l));
  };
  std::uint32_t gpr[32] = {};
  gpr[1] = sp;
  gpr[25] = obj;
  std::uint32_t pc = 0x80032E44;

  // Hung as the PS2 hangs them (read from its RAM in a fight): the forearm scaled by his weapon scale, turned
  // 180 degrees about the arm on the right and not at all on the left, moved (0.144, 0.0192, 0) in its own
  // axes; then the hold his halves were built through (build_heihachi.GRIP, rows of R then t). The hand's
  // bend and twist do not move it, nor do the twist bones (junk below), nor does whatever the game left in
  // the mount rows: the hand times its hold in most moves, the PS2's band itself in a punch.
  const Mat hold_r{0, 0, -1, 0.0768f, 0, 1, 0, 0.0384f, 1, 0, 0, 0, 0, 0, 0, 1};
  const Mat hold_l = Turn(80, -0.03744f, 0.0576f, 0, 2);
  const auto ps2_band = [](const Mat& fore, float turn) {
    Mat local = Turn(turn, 0.144f, 0.0192f, 0, 0);
    for (int i = 0; i < 12; ++i)
      if (i % 4 != 3)
        local[i] *= 1.0368f;
    return Mul(fore, local);
  };
  for (const auto& [tw_r, tw_l] : {std::pair<float, float>{179, 162.4f}, {92, 96}, {-150, 120}, {20, -40}})
    for (const bool punch : {false, true})
    {
      const Pose p = pose(tw_r, tw_l, 0, 0);
      Ram him;
      setup(him, 8, 12, p);
      if (punch)  // the game already hung the right band from the forearm
        him.Put(rows + 23 * 64, ps2_band(fore_r, 180));
      for (std::uint32_t row : {153u, 154u, 155u, 156u, 177u, 178u, 180u, 181u})
        him.Put(rows + row * 64, Turn(37, 0.3f, -0.2f, 0.1f, 1));
      CHECK(ApplyHook(*hook, both, gpr, pc, 0, him.Read(), him.Write()) && pc == 0x80032E44);
      CHECK(Near(him.Get(rows + 23 * 64), Mul(ps2_band(fore_r, 180), hold_r)));
      CHECK(Near(him.Get(rows + 24 * 64), Mul(ps2_band(fore_l, 0), hold_l)));
      // The skeleton runs up to three times a frame; a second pass over the same rows changes nothing.
      const std::vector<std::uint8_t> once = him.bytes;
      CHECK(ApplyHook(*hook, both, gpr, pc, 0, him.Read(), him.Write()) && him.bytes == once);
      CHECK(Near(him.Get(rows + 9 * 64), p.te_r));  // the bones stay
      CHECK(Near(him.Get(rows + 8 * 64), fore_r) && Near(him.Get(rows + 22 * 64), Mat{}));
    }

  // Anyone else's weapon (another case's parents), or Heihachi not installed: untouched.
  for (const auto& [right, left, installed] :
       {std::tuple<int, int, Installed>{9, 14, both}, {9, 13, both}, {8, 12, spawn}})
  {
    Ram other;
    setup(other, static_cast<std::uint8_t>(right), static_cast<std::uint8_t>(left), pose(92, 96, 70, 74));
    const std::vector<std::uint8_t> before = other.bytes;
    CHECK(!ApplyHook(*hook, installed, gpr, pc, 0, other.Read(), other.Write()) && other.bytes == before);
  }
}

// His select tile: the grid puts cell c at column c % 5, row c / 5, so cell 9 lands under Spawn. Every
// place that computes a cell's position (the tile, the cursor frame, the helper at 801518C8) gets his
// moved to column -1 of row 0, left of Mitsurugi: opposite Spawn, who sticks out right of the same row.
static void HisTileSitsLeftOfTheTopRow()
{
  const Installed both{true, true}, spawn{true, false}, heihachi{false, true};
  const auto put = [](Ram& ram, std::uint32_t a, float x, float y) {
    std::uint32_t u;
    std::memcpy(&u, &x, 4);
    Write32(ram.Write(), a, u);
    std::memcpy(&u, &y, 4);
    Write32(ram.Write(), a + 4, u);
  };
  const auto get = [](Ram& ram, std::uint32_t a) {
    std::array<float, 2> v;
    for (int i = 0; i < 2; ++i)
    {
      const std::uint32_t u = Read32(ram.Read(), a + i * 4);
      std::memcpy(&v[i], &u, 4);
    }
    return v;
  };
  const std::uint32_t base = 0x80350000;
  for (const auto& [pc, reg, off] : {std::tuple<std::uint32_t, int, std::uint32_t>{0x8015196C, 3, 0},
                                     {0x80151C44, 29, 80}, {0x80153244, 3, 208}})
  {
    const SpawnHook* const hook = FindHook(pc);
    CHECK(hook != nullptr);
    if (!hook)
      continue;
    std::uint32_t gpr[32] = {}, at = pc;
    gpr[reg] = base;
    for (const Installed& on : {both, heihachi})
    {
      Ram ram;
      put(ram, base + off, 480, 144);
      CHECK(ApplyHook(*hook, on, gpr, at, 0, ram.Read(), ram.Write()) && at == pc);
      CHECK((get(ram, base + off) == std::array<float, 2>{160, 82}));
    }
    Ram absent;  // without him cell 9 is empty, and nothing is drawn there to move
    put(absent, base + off, 480, 144);
    CHECK(!ApplyHook(*hook, spawn, gpr, at, 0, absent.Read(), absent.Write()));
    CHECK((get(absent, base + off) == std::array<float, 2>{480, 144}));
    Ram other;  // any other cell (Spawn's, the one left of his old place) keeps the game's own
    for (const auto& [x, y] : {std::pair<float, float>{480, 82}, {416, 144}})
    {
      put(other, base + off, x, y);
      CHECK(!ApplyHook(*hook, both, gpr, at, 0, other.Read(), other.Write()));
      CHECK((get(other, base + off) == std::array<float, 2>{x, y}));
    }
  }
}

// The cursor walks cell numbers: +-1 within a row (wrapping), +-5 between rows, skipping empty cells. At
// 801530A4 the move is done (r31 the old cell, r29 the new one, already in the cursor's +4) and the game
// is about to hover the new tile and release the old one; the hook rewrites the move as if his cell were
// left of Mitsurugi, so that hover/release still runs for the right tiles.
static void CursorReachesHimFromTheTopRow()
{
  const Installed both{true, true}, spawn{true, false}, heihachi{false, true};
  const SpawnHook* const hook = FindHook(0x801530A4);
  CHECK(hook != nullptr);
  if (!hook)
    return;
  constexpr std::uint32_t kUp = 0x2000, kDown = 0x1000, kLeft = 0x800, kRight = 0x400;
  const std::uint32_t cursor = 0x80350000;
  struct Move
  {
    Installed on;
    std::uint32_t old_cell, game_cell, buttons, want;
  };
  for (const Move& m : std::initializer_list<Move>{
           {both, 0, 4, kLeft, 9},       // Mitsurugi, left: him (the game wraps to Spawn)
           {both, 9, 5, kRight, 0},      // him, right: Mitsurugi (the game: 9 -> 10 -> wrap to 5)
           {both, 9, 8, kLeft, 4},       // him, left: wrap to the row's far end, Spawn
           {both, 4, 0, kRight, 9},      // Spawn, right: wrap to him
           {heihachi, 9, 8, kLeft, 3},   // without Spawn the row ends at cell 3
           {heihachi, 3, 0, kRight, 9},
           {both, 9, 4, kUp, 25},        // him, up/down: column 0's neighbours (last row 25..28)
           {both, 9, 14, kDown, 5},
           {both, 8, 9, kRight, 5},      // row 1 no longer holds him: wrap past his old cell
           {both, 5, 9, kLeft, 8},
           {both, 4, 9, kDown, 4},       // nor does Spawn's column: nothing below Spawn
       })
  {
    Ram ram;
    ram.bytes[cursor + 2 - 0x80300000] = 0, ram.bytes[cursor + 3 - 0x80300000] = 28;  // +2: the last cell
    ram.bytes[cursor + 5 - 0x80300000] = static_cast<std::uint8_t>(m.game_cell);
    std::uint32_t gpr[32] = {}, pc = 0x801530A4;
    gpr[26] = cursor, gpr[28] = m.buttons, gpr[29] = m.game_cell, gpr[31] = m.old_cell;
    // A hook that changes nothing says so.
    CHECK(ApplyHook(*hook, m.on, gpr, pc, 0, ram.Read(), ram.Write()) == (m.want != m.game_cell) && pc == 0x801530A4);
    CHECK(gpr[29] == m.want && ram.bytes[cursor + 5 - 0x80300000] == m.want && ram.bytes[cursor + 4 - 0x80300000] == 0);
  }
  // Moves that never touch his cell, and any move without him: the game's own.
  for (const auto& [on, old_cell, game_cell, buttons] :
       {std::tuple<Installed, std::uint32_t, std::uint32_t, std::uint32_t>{both, 1, 0, kLeft},
        {both, 6, 1, kUp}, {both, 3, 4, kRight}, {spawn, 0, 4, kLeft}, {spawn, 8, 5, kRight}})
  {
    Ram ram;
    ram.bytes[cursor + 3 - 0x80300000] = 28;
    ram.bytes[cursor + 5 - 0x80300000] = static_cast<std::uint8_t>(game_cell);
    const std::vector<std::uint8_t> before = ram.bytes;
    std::uint32_t gpr[32] = {}, pc = 0x801530A4;
    gpr[26] = cursor, gpr[28] = buttons, gpr[29] = game_cell, gpr[31] = old_cell;
    CHECK(!ApplyHook(*hook, on, gpr, pc, 0, ram.Read(), ram.Write()) && gpr[29] == game_cell && ram.bytes == before);
  }
}

// The screen's exit animates each tile by its column (cell % 5, in r0 at 801521B8): 0 and 3 slide up, 1
// and 2 down, and each ends in state 6. Column 4 only turns and never ends, as no stock tile is in it; his
// cell 9 was, and spun on over the stage select. He sits left of column 0 now, so he leaves as column 0.
static void HisTileLeavesLikeTheLeftColumn()
{
  const Installed both{true, true}, spawn{true, false}, heihachi{false, true};
  const SpawnHook* const hook = FindHook(0x801521B8);
  CHECK(hook != nullptr);
  if (!hook)
    return;
  const std::uint32_t tile = 0x80350000;
  for (const auto& [on, cell, want] : {std::tuple<Installed, std::uint32_t, std::uint32_t>{both, 9, 0},
                                       {heihachi, 9, 0}, {spawn, 9, 4}, {both, 4, 4}, {both, 8, 3}})
  {
    Ram ram;
    Write32(ram.Write(), tile + 120, cell);
    std::uint32_t gpr[32] = {}, pc = 0x801521B8;
    gpr[28] = tile, gpr[0] = cell % 5;
    CHECK(ApplyHook(*hook, on, gpr, pc, 0, ram.Read(), ram.Write()) == (want != cell % 5) && gpr[0] == want);
  }
}

int main()
{
  OldManifestAsksForARebuild();
  GuestsHashIsTheManifests();
  CodeOfAnAbsentGuestPassesThrough();
  ManifestValidEnables();
  ManifestWithXboxRecordValidates();
  ManifestWrongDiscRejects();
  ManifestHashMismatchRejects();
  MissingArchiveRejects();
  ArchiveSizeMismatchRejects();
  GarbledOrFutureManifestRejects();
  ResolveOffReturnsOriginal();
  ResolveOnReturnsSpawnArchive();
  ResolveOtherFilesUntouched();
  ResolveUnderAFilesFolder();
  ResolveRelativeGameRoot();
  ResolveNormalisesThePath();
  PathsFollowTheGameRoot();
  ManifestTablesParsed();
  ManifestBadTablesRejects();
  PatchesCoverEveryTable();
  MovedSpawnData();
  ManifestCellMustBeSpawn();
  PatchesWriteWhenExpected();
  PatchesAllOrNothing();
  PoolsMismatchWritesNothing();
  PatchesWaitForTheDol();
  OffWritesNothing();
  SitesAreTheClassifiedOnes();
  SiteSwapsOnlyAnInstalledGuest();
  SiteIgnoresAnUnexpectedInstruction();
  HooksAreTheClassifiedOnes();
  TwoGuests();
  GauntletsHangAsOnThePs2();
  HisTileSitsLeftOfTheTopRow();
  CursorReachesHimFromTheTopRow();
  HisTileLeavesLikeTheLeftColumn();
  AFighterWithNoRowTakesTheLastOne();
  CodeLookupReadsTheTable();
  HisCodeReadsBackAsHim();
  RecordGetsHisCodeOnlyForHim();
  PlayCountWritesNoRowForHim();
  WinLossCountsOnlyRealRows();
  RepairsOnlyTheKnownDamage();
  if (g_failures)
  {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("guests: ok\n");
  return 0;
}
