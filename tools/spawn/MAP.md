# Spawn data map (US disc GRSEAF unless stated)

Every location here was confirmed by a measurement, named next to it. Tools used:
a local `readmap.py` helper, not in this repo (maps a logged read to group/entry/frame), and an
`LD_PRELOAD` shim logging `pread` on `root.olk`, `movie.afs` and `bgm.afs`.

## Harness trap that hid the ending

Action Replay codes do nothing unless the user dir's `Config/Dolphin.ini` has `[Core] EnableCheats =
True` (default false; only the in-game menu sets it). Without it the arcade-final route loses on stage 3
(ring-out vs Astaroth). Check: a held AR write must change the RAM-hash column versus a run without it.

## `root.olk` groups

| group | records | content |
|---|---|---|
| 1 | 33 | small per-stage items, one read before each fight |
| 2 | 516 (Xbox 585) | UI: textures (`VGT.` GC / `VXT.` Xbox) and `mmg`/`mmx` containers, each item stored once per platform at neighbouring indices |
| 3 | 410 (Xbox 404) | per-stage pairs (~2–6 MB + 16 KB), Xbox and GC versions interleaved |
| 4 | 982 | 0–45 stages (GC 16/32 = Link's stage, Xbox 17/33 = Spawn's); 46+ character blocks of 39 (block b = 46 + 39 b) |

Character blocks seen in the cheats-on arcade trace: Talim 14; opponents 15, 10, 11, 8, 12, 0, 17, 18 (+
Inferno borrowing 5 and 2). Link is block 23 on GC; Spawn is block 23 on Xbox.

Spawn's block is complete: every slot Talim uses (0, 1, 8, 9 costumes; 16–27 weapons; 28–29 motion;
30–36 `mmx` containers) has an Xbox Spawn entry, and the spike converts all of them.

## Ending (found: cheats-on trace, frames 78,657–86,000, screenshots + reads)

- Inferno falls ~78,657; in-engine scene in a stained-glass room ~80,465 (stage group 4 entry 41, read
  78,322, shared); illustrated pages with scrolling text from ~82,273; music `bgm.afs` entry 10.
- **Illustrations: group 2, entry 57 + character ID** (Talim 22 → 79 read at 81,225; GC Link 27 → 84;
  Xbox Spawn 29 → 86). **GC record 86 exists and is empty → Spawn's art is a replacement, not an append.**
  Formats and the GC palette layout: `ui.py` docstring. Converter oracle (Talim Xbox → GC): PSNR 26.6 /
  27.6 dB on the two pages (GC requantised its own palette).
- **Text: group 2 entry 87**, read with the art; byte-identical on both discs and **already contains
  Spawn's (and Link's) ending text**. Nothing to convert.
- **Ending table, `0x802A3748`, 8 bytes per ID** (Task 9): s16 illustration entry (group 2), s16 page
  count, u32 first text page. GC row 29 is all -1s; the Xbox's is (86, 11, 85). Read at `8016D8D4` (the
  ending's setup, `8016D83C`, index = character ID at +116 of the ending object `*0x802D3350`). With -1
  the size call `800179A0` returns the size of the whole group 3, the buffer after it is placed past the
  end of RAM (`0x8AA14880`), and its memset at `800054FC` crashes the game after the final KO. The
  runtime writes the converted Xbox row (manifest `ending`, checked against `ending_old`).
  Verified: windowed arcade from the user's state, his scene, then four pages ("The Hellspawn fought his
  way…" to "…with nothing in his grasp."), then Game Over and the rankings.
- Patches are written once at boot. **A savestate taken before the fix restores the -1 row**, so it
  still crashes at the ending; a state saved on the fixed build carries the row.
- Stage 7, read before Inferno, is the player's HOME stage (site `800B5628`): an ungated module hangs
  there (stage record 7 is empty). Only the gated module reaches the ending.

## Select grid (Task 8)

- **Cell records**: 30 x 36 bytes at `0x80250520` (built by code at 80149B60), index = row*5 + col, 6 rows
  of 5. Column 4 of every row is an end marker (ID -1); cell 26 is the random cell (ID -2). Record: u16 ID,
  u16 face count n, u32 draw callback, bytes 8..15 = face indices ("A"), 16..23 = silhouette indices ("B").
  The cursor object holds min/max index at +0/+2, the current index at +4, and one enabled byte per cell
  at +12; navigation (80154844..) skips cells whose byte is 0.
- **His cell = index 4**, the free end of Link's row, drawn right of the frame (user's choice, 2026-11-03;
  a 7th row is off-screen: rows sit at y = 82 + 62*row on 640x448). The patch writes Link's record with ID
  29 and his two faces; the end marker moves nowhere (row 0 simply has five cells). UP from Talim reaches
  Mitsurugi, LEFT wraps onto him; from Raphael, UP reaches Taki, RIGHT reaches him. UP/DOWN stay on him.
- **Highlighted ID**: P1 `0x8034D550` (+2 = costume), P2 `0x8036E5F0`. On the random cell the field keeps
  the previous ID.
- **Random select** (confirm handler near 8014CCC8, `cmpwi r23, 26`): collects every cell whose ID is > 0
  and whose sprite object (r27+112, entry 64+i) has neither the player's bit at +111 nor the +112 flag set,
  then picks `rand() % count` and moves the cursor there (vtable +44). His cell is in that pool with no
  further change.
- **Face bank** (portraits): group 2 entry 5, part 2: a VGT of 77 64x64 textures, C8 except 73/74 (C4).
  Header +8 = n, +0x10 = table offset (0x18), +0x14 = end of the descriptor area; 0x44-byte records;
  each record's word 0 points to a 24-byte TLUT descriptor `(0, offGR, count<<16, 0, offAB, count<<16)`.
  Palettes are split: (G,R) then (A,B), so colour = (R=A.lo, G=A.hi, B=B.lo, alpha=B.hi), and they form one
  block before the first pixel data. Unused slots: none (33 Inferno, 73/74 C4 UI, 75 "?", 76 empty frame),
  so his two faces are appended as 77 and 78 (`ui.face_bank`).
  **The game's loader converts the bank in place and needs**: records contiguous at 0x18 (it ignores the
  +0x10 offset); descriptors as one array right after the records, ending at +0x14; palettes inside the
  palette block before the first pixel data. Violating any one blanks his face AND the whole top row.
- **Name plate (DEFERRED, user's call)**: shows "Q.TAKAGI". Slot 29 is the unused dummy character
  Q.Takagi on PS2/GC (Cassandra's 2P model, Mitsurugi's moves, Kilik's weapon); GC keeps her
  pre-rendered plate where Xbox has Spawn's. Not a string (all TALIM copies poked: plate unchanged), not
  text in RAM in any encoding, not a dumped texture, not an EFB copy. Lead: `0x80250958` maps ID -> cell
  sprite (29 -> 1; poking it to 5 showed TAKI and Taki's model); the setter is the vtable method at
  80153300. Dummy IDs are known to crash endings/galleries on these discs: check those in Task 10.
- **Silhouettes** ("B" bytes) are still Link's (100, 101).
- **Costume size budget**: a costume bigger than GC's largest (Xianghua slot 1: model part 1,178,880,
  entry 1,959,296 bytes) crashes the fight load (Invalid read 0xc at 8003a138 / 80033ad4, a null object
  at obj+24). His costume 2 converts to 1,245,696 / 2,003,840; costume 1 padded to that size crashes the
  same way. `build_spawn.fitted_model` drops the top mip of the largest multi-level textures until the
  model fits (costume 2: 512x256 DXT5 -> 256x128).
- Select-screen pose and model follow the ID: no table needed.
- VS art, move-list text: not traced yet (Task 10).

## Character pools (Task 9)

- **Block `0x8028B50C`–`0x8028B603`** (248 bytes): 13 ID lists, each 255-terminated and padded to 4.
  **Pointer table `0x8028B604`**, 43 words: entries 0, 1, 12 and 16–41 point into the block (17 of them at
  the first list), the rest at short lists in `0x80460540..` (sdata). No code loads either address
  directly (`lis/addi` scan): reached through another pointer, not yet traced.
- The first list is the whole cast (1, 4, 26, 5, … 24, 27, 30, 31, 32). The Xbox executable holds the same
  lists (XBE `0x36F4EC`) with **29 exactly where GC has 27**, in all seven long lists that carry Link;
  GC's one-entry `[27]` is the Xbox's `[29]`, and GC's short `3 22 27` is the Xbox's `3 22` (both left).
- Patch: `tables.repack_pools` inserts 29 after 27 in those seven lists and packs the 13 lists unpadded
  (236 of 248 bytes); the table entries follow their lists. Byte-aligned starts are safe: the Xbox packs
  some of the same lists unpadded. **Not the arcade**: with every list set to `[29]` the arcade's
  opponents were unchanged; which mode reads them is not known yet.

## Arcade opponents (Task 9)

- Pick at `80089E98`: count IDs 0–32 whose status (`80145AA0`) is ≥ 2, then draw `rand * 33` until one
  qualifies (and is not the current one). Status: 0 if the costume gate (`800A8E04`) refuses the ID; else
  the s16 unlock index at `0x8029F1F8 + 2*id`: -1 → 3 (always), otherwise the save's unlock byte at
  `0x80402160 + index`. GC ID 29 = -1 (Xbox: 47, which on GC is Link's flag), so **he is in the pool as
  soon as his costume row is patched** (Task 6). No arcade patch.
- How it was found: the opponent's ID lands in the player object at `0x8036E560 + 0x62` (P2 object;
  written by `800A16CC` in the setup function `800A164C`, which is the costume gate's caller); P2's select
  field `0x8036E5F0` stays at the select default (21) for the whole arcade. The setup's caller
  `80099024` has 25 `bl` sites, mostly in the 8008xxxx mode code; one, `80089F84`, follows the pick.
- Not the arcade either: 3-ID records (8 bytes, used flag at +6) at `0x8027B4DC` (16), `0x8027B564` (13),
  `0x8027B5D4` (4), picked at `800B8258`. They match the Xbox's except six records where GC has Link in
  Spawn's place. Left alone.
- Ending: illustrations are group 2 entry 57 + ID (his = 86, converted in Task 2); text entry 87 is shared.


## Per-character tables in `main.dol` (Task 6)

IDs index every table directly (ID 0 is unused). On GC, IDs 7–10, 14, 16, 28 and 29 are empty in all of
them; the Xbox executable has the same tables, the same layout, Link's slot (27) empty and Spawn's (29)
filled. All are DOL data, unchanged at runtime (checked against RAM at frame 6000). `tables.py` converts
Spawn's Xbox rows; the runtime writes them (`RecompGuestTables.h`) after checking each slot is empty.

| table | address | per ID | how it was found |
|---|---|---|---|
| block | `0x80277A58` | s16: first group-4 record (Link 943, Spawn → 982) | `lhax` at `80098A48`; -1 at every empty ID |
| roster | `0x80250958` | u8 (empty IDs hold 19); Link 1 on GC, Spawn 1 on Xbox | sentinel scan; same bytes in the XBE at `0x172618` |
| costume | `0x80279484` | 6 bytes: costume selectable | **the gate**: `800A8E04` reads it; 0 ⇒ `800A164C`/`800A4C50` replace the ID with 23 (`li 31, 23`) |
| descriptor | `0x80275A60` | 0xF8 bytes: 5 string pairs (name, caps name, weapon, origin, age) at 0x18 strides, 16-bit fields 0x78–0xA8 (model IDs at 0x82/0x86/0x88), floats 0xA8–0xB8, bytes after | the record read at `800A4C80` after the gate; converter oracle reproduces all 23 shared rows |
| (string area) | `0x802761E8` | 56 bytes | the tail of empty descriptor row 7, not a pointer slot; holds Spawn's strings |

How the gate was found: holding P1's select field at 29 still loaded another fighter; a temporary Jit64 +
Dolphin memcheck build (env-armed, since removed) named the writer `800A4D14` and the check before it.
The module's write journal cannot do this: MEM_FAST modules compile the journal out.

P1's character on the arcade route never comes from the grid: `800AE8CC` stores a hard-coded 22 into
`0x8034D550` (P1 default character; costume at +2), and `8015833C` passes it to player creation
(`80098C7C`). `RINGOUT_SPAWN_FORCE_ID=29` holds that field.

Verified (arcade route, force 29): HUD reads SPAWN vs XIANGHUA, his block 24 loads, his intro plays, the
round timer runs; off is RAM-hash-identical to stock over 6000 frames. Intro line shows ".A": that text is
not in the descriptor (Task 8).

Windowed-capture trap met here: `import -window` returned identical PNGs for frames 4700/4900 in 4 of 5
forced runs while the game ran on (timer counting in a headless dump); the 5th, with the window placed on
DP-1, moved. Check a headless state value before calling a windowed freeze a game hang.

## Per-character code sites (Task 7)

`.github/scripts/spawn-id-scan.py main.dol` lists every compare against 27/28/29 and every jump table
bounded at 27–35 cases within 32 instructions of a load of a character-ID field: **35 sites** on the US
DOL (22 compares against Link's 27, 2 against 28, 1 against 29, 10 switches). Each was read by hand:

| site | what | ID 29 on GC | Xbox (Spawn) | ruling |
|---|---|---|---|---|
| 22 × `cmpwi rX, 27` | Link-only paths (hookshot etc.) | not-Link path | n/a (no Link) | none: he is not Link |
| `800918DC`, `80092098` (`cmpwi 28`) | range split around 25/27 | the ≥ 28 path, as 0–24 | — | none |
| `800CE544` (`cmpwi 29`) | **a leftover Spawn branch** in the GC executable | his own path again | his own | none: it is his |
| `80058EE8` (0–31) | per-ID flag | table has a 29 row (1) | — | none: the row is there |
| `800F3264`, `800F4490`, `800F5F4C`, `800F6028`, `800F85CC` | tables that cover 29 | same case as 22 | — | none |
| `800A661C` (0–26) | per-ID value, Link (27) takes the default 1 | default 1, as Link | no counterpart found | none (as Link) |
| `800834DC`, `80083594` (0–27) | value 504–510 | default 504 | **509**, the case of 24/25 (index table at Xbox `0x5AAEC`) | site → 24 |
| `800B5628` (0–27) | home stage: value + 7 is a group-4 scene record (Link 9 → 16/32) | default 0 → records 7/23 (empty on GC) | **10 → 17/33**, his own scene | site keeps 29; the build puts his scene in 7/23 (was → 22, which played Talim's scene) |

The Xbox side was matched by finding MSVC's two-level switch (a byte per ID indexing a small jump
table) whose grouping of the 20 shared IDs equals the GC table's; `xswitch.py` in the scratch tree.

At a site the module returns to the dispatcher (gate group 1, armed only while Spawn is on and his
tables are patched); `ApplySite` swaps 29 for the site's ID in the compared register, for that one
decision. Gated PCs are now also entry cases (they are mid-block compares, not leaders).

## Character codes: rankings and per-character stats (2026-11-05)

Rankings and the CHARACTER DATA stats file a fighter under a **code 0-23**: its index in the 24-entry
s16 table at `0x80274D10` = {21,22,26,23,1,3,5,17,18,11,12,13,4,25,27,6,2,15,24,20,19,30,31,32} (the
Xbox's has 29 where GC has Link's 27). ID -> code is `800914C4` (scans 24, miss -> -1); code -> ID is
`80091494` (clamps to 0-23, ~80 callers). There is no 25th slot: the stats object (`0x80405D38`)
has 24 rows of 28 bytes and other fields from +0x2AC, and it is saved.

- **Ranking records** (`0x803EF424`, 28 bytes; code byte at +0, name at +9, time at +0x15) are
  built at `8011D3D4` from the fighter's ID (player object +0x58); the code byte is `8011D440`'s
  lookup. Spawn got -1 there. The face is `80158688(ID, costume)` through the select tables, the
  name `80115F6C(ID)`, both of which already know 29 -- only code -> ID lost him.
- **Stats writers** index rows with the code unchecked: play count `80178BD4` (+8) and wins/losses/
  draws `80178C50` (+12/+20/+28, a column per mode). With -1 they wrote in front of the table:
  every Spawn session's RAM shows `...09 0b 0a 08` at `0x80405D18` turned into `...09 0b 00 01 86 9f`
  (the 99999 cap), absent in every stock dump.
- Team records keep 0xFF as an empty member (`8011DBA4`), so 0xFF cannot be his code.

Fix (RecompGuestTables.h `kUsHooks`, gated group 1): his ranking/team records get code **24**, read
back as 29 at `80091494`; the two stats writers skip rows outside 0-23 (he has no row, his opponents
keep theirs). Read-only out-of-range uses of -1 (`80145544`, `80145D9C`, `80147034`, `801471A8`) are
left as the game had them.

**Name entry (2026-11-09).** Code 24 only makes a row *display* as him; it cannot create one. Each
per-fighter board (categories 0-3, 6-9) has exactly 20 rows, one per fighter, codes 0-19. `8011D3D4`
looks the code up in the first N rows (N = unlocked fighters) and returns -1 at `8011D57C` when absent,
and `800B69C4` then skips name entry. It runs twice per clear: `r27` != 0 only asks for the rank
(`800B74FC`), `r27` == 0 writes (`800B75C4`). The hook at `8011D574` makes the last row his to beat; the
game's compare and shift do the rest. A fighter dropped this way returns by the same route. There is no
21st row: the boards are packed end to end in the save. Arcade also skips the ranking when player
+0x1540 != 0 (`80088958`).

The user's own rank-1 record ("AA", 2'58"35) holds code 1 = Talim. An ID-29 clear stores 0xFF (the
lookup's -1), so that record was most likely made while he still ran in Talim's slot (the spike); it
keeps showing Talim. Not proven: no arcade clear as 29 has been driven yet.

## Weapon Master and weapon unlocks (Task 10)

- **Weapon unlock table, `0x802A81E0`, 0x78 bytes (60 s16) per ID**: each value is an index into the
  save's unlock bytes (`0x80402160`, 276 bytes, 3 = unlocked); -1 = no weapon there. Row 29 holds
  12 weapons on slots 253–263 (weapons 0 and 1 share 253); Link (27) uses 218–229, separate slots.
- **Oracle: GC == Xbox for every row 0–32, including 29** (XBE file offset `0x330F58`, found by rows 1–6
  little-endian, unique). GC kept Spawn's row and save slots, so nothing is converted or patched; his
  weapons unlock through the same card slots as on the Xbox. A save with everything unlocked (all 3s)
  gives him all 12.

## Voice language (Task 10)

- Block slots 30–32 are the Japanese voice sets, 34–36 the English ones (same clip counts per fighter),
  33 is shared. The entry function `80098A04` returns `block_start[id] + slot` and adds 4 to the slot
  (not 33) when the voice language is English **and the byte at `0x80277A9C + id` is set**.
- GC ships 0 for 29 (the dummy Q.Takagi had only Japanese); the Xbox has 1. With 0 he spoke Japanese
  while everyone else spoke English (user report). The runtime writes his Xbox byte (manifest `voice`).
  Verified with the user's English card: Talim loads slot 34, Spawn slot 30 before and 34 after.
- `80058EE4` (a per-ID 0/1 switch that the byte at +204 inverts) is not the language: 29 returns 1
  with the female fighters (Takagi was Cassandra's model). Not patched; checked against nothing yet.

## Name (Task 10)

0x8028E9D8 holds a pointer per ID to a kerned name string (`$lN` = move left N, `$d1`/`$u1` = down/up
1). The VS screen, select screen and HUD draw it. (0x80250958 is a separate per-ID index, not the name.)
GC 29 points at the dummy's `Q.$l4T$l9AKA$l4GI` (0x8028ED7C). His Xbox string is `S$l4P$l12A$l6$d1W$u1N`
(22 bytes), too long for that slot, so it goes in empty descriptor row 8 (+0xC0, `0x802762E0`) and pointer 29
moves there. The shipped strings of 28 (Heihachi) and 29 stay as they are. tables.spawn_name finds the XBE table by its pointers to IDs 1-6
and checks every shared ID's string. Verified on screen: the VS screen reads SPAWN (spawn-built9).

## Weapon-select preview (Task 10)

- The preview is its own model (the weapon VMG, loaded from his block's entry 16 + weapon by the state
  machine at `800A3798`). Its setup (`800A3AEC`) parks all 32 matrices at y = 100 (`800A3D98`); each frame
  `8017E998` poses them from **`0x80251904`, per ID {u32 count, u32 pointer} to 28-byte bone records**
  (u32 parent, float xyz, float rotation xyz). GC ships count 0 for 29, so his preview stayed parked and
  Agony never showed (user report). As ID 22 he used Talim's row, which is why it once worked.
- **Oracle: GC == Xbox for every shared row** (XBE table found by ID 1's records). Xbox row 29 = 1 record.
  It goes into 28 unused bytes of empty descriptor row 7 (+0x64: no pointer slots, clear of the string
  area at +0xC0); row 29 points there. Manifest key `preview`.
- Dead ends on the way, for the record: the select pose (a shared script in g4 e1 part 0 maps ID -> pose
  move, 29 -> e2 motion #37, byte-equal to the Xbox), the descriptor row, the projection/view (identical
  for all), and the bone-5 camera target are all innocent. GX prim counts per interval were what showed
  the weapon was never drawn rather than drawn off-screen.

## Pre-fight lines (Task 10)

- Costume part 6 holds the fighter's lines for the stage-7 "Destined Battle" scene and the other
  pre-fight exchanges: u32 columns, u32 lines, u32 offsets, NUL-terminated strings, little-endian on both.
  The Xbox has six strings per line (column 0 and 1 English, then German, French, Spanish, Italian) under
  the header word 0x10; the US GC keeps column 0 alone under 1. Parts 4 and 5 (the scene scripts) are
  byte-identical on both platforms.
- Copied raw, his scene showed the German copy of another line ("diesen Mist.") against Raphael (user
  report 2026-11-04). `xb2gc.dialogue` keeps column 0; oracle 68/68 costumes byte-exact.
- Verified from the user's stage-7 state (RAM patched, since the state holds the old part): "You know
  about Soul Edge." / "You! Answer me!" against Raphael's "Phantom!" / "What do you want of me?!".

## Home-stage scene (Task 10)

- Group-4 records 8–45 are not stages: each is one fighter's home-stage scene (the stage-7 "Destined
  Battle"), record = switch value + 7 (Japanese) or + 23 (English, the flag at `+0x205A0` of
  `0x8038F640`). Part 0 a two-motion bank, part 1 the motion records (u16 ID first: 29 in his) + a
  Shift-JIS table, part 2 a u32 table with four u16 at 0x48, parts 7/8 the scene's voice banks.
- Site → 22 gave him Talim's record 27: the AX log matched the scene's clips to Talim's (block 14 slot 36)
  and Yunsung's voice banks (user: "wrong voice and animation", 2026-11-04).
- Fix: `xb2gc.scene` converts Xbox 17/33 into GC 7/23 (the default's records, empty on GC); the site's
  `as_id` is 29 (it must stay: the module is gated there). Oracle: parts 1/2 byte-exact on 24/24; part 0
  everywhere but filler between motion blocks, which the GC leaves stale.
- **Inserting records shifts every later group-4 offset, so a savestate from an older build breaks.**
- The scripted arcade route cannot reach stage 7 as Spawn: its Talim inputs ring him out on stage 6.

## Character Profile camera

Group 2 entry 503 on GC (Xbox 516; 517-520 hold the same Spawn data). A 96-part pack, three 32-slot
sections by character ID: section 0 is f32 (u32 swap), sections 1-2 a u32 count then u16s. The GC has
Link in 27/59/91 and nothing in 29/61/93, so the profile camera's goal (camera object 0x80851830 +0x50,
eased into +0x100 by 800126B4) stayed zero and aimed at his feet. `ui.profile_camera` fills his slots.

## Select-screen silhouettes

Group 2 entry 10 part 0: a VGT bank of 128x256 C4 white figures, one per costume, drawn while a fighter's
model loads. A select cell's +16..+23 bytes index it (GC Link 100-103, Mitsurugi 50-52; the Xbox bank
has 6 more early textures, so Xbox indices are GC + 6 for shared fighters). His cell was Link's copy, so
he showed Link's figure. His Xbox figures 106/107 use palette entries 0-15 only and convert exactly to C4;
`ui.silhouette_bank` appends them as 150/151 (same rules as the face bank, via `ui.append_textures`).
The cell's +28..+35 (f32, u16, u16) also come from his Xbox cell: identical across discs for shared fighters.

## US Xbox release (2026-11-04)

The US disc (version 2049, region 7; `root.olk` sha1 `5e342e6e…`, executable `e5d4fb1f…`, region-free
patched copy `e32386cc…`) converts with the PAL map except where `releases.US` says otherwise. Each entry
was located by matching PAL's bytes:

| What | PAL | US | How confirmed |
|---|---|---|---|
| his group 4 block | 943-981 | same | 33 of 39 records byte-identical; costumes 0/1/8/9 differ only in part 6, movesets 28/29 in part 0 (a balance revision; the moveset oracle still converts 40/40) |
| pre-fight lines (costume part 6) | 6 columns, header 0x10 | 1 column, header 1: already the GC layout, zero-padded | costumes 0/1: equal to PAL's converted lines plus padding; the dialogue oracle 68/68 |
| home-stage scene | 17, 33 | same | byte-identical |
| ending art | 57 + ID | same | entry 86 byte-identical |
| face bank | 2/5, Spawn 51/52 | same entry and indices | the textures are byte-identical to PAL's 51/52 |
| silhouette bank | 2/10, Spawn 106/107 | 2/10, Spawn 100/101 | 148 textures instead of 154; PAL 106 and 107 (the same figure) equal US 100 and 101 |
| profile camera | 2/516 | 2/503 | the profile-camera oracle 53/55 with the same two known misses |

`tests/test_oracle.py` runs every oracle on both releases (`USOracleTest` reads the image), and
`tests/test_build.py USBuildTest` builds from the image and from the extracted files (same archive).

## Heihachi, ID 28 (2026-11-09)

Source: the US PS2 disc, `SLUS-20643` 1.00. Its executable holds the same per-ID tables, little-endian, with
every row of the shared cast converting to GC's (`tools/heihachi/ps2tables.py` checks them each build).

Where his rows go: his own slots for ID 28; strings in empty row 9 from +0x64, preview records in empty row
10 +0xC0, neither a pointer slot. GC kept his kerned name (`0x8028ED6A`) and his weapon rows and save slots
243-252, so those need nothing. Cell 9. Ranking code 25.

Sites, re-read for 28:

| site | 28 | ruling |
|---|---|---|
| 22 x `cmpwi rX, 27` | not-Link path | none |
| `800918DC`, `80092098` (`cmpwi 28`) | the >= 28 path, as 29 | none |
| `800CE544` (`cmpwi 29`) | not taken | none |
| `80058EE8`, `800F3264`, `800F4490`, `800F5F4C`, `800F6028`, `800F85CC` | the GC table has a row for 28 | none |
| `800A661C` (0-26) | default 1, as Link | none |
| `800834DC`, `80083594` (0-27) | default 504; PS2 gives him **509** (table `0x48B5C0`, IDs 1-28), the case of 24 | site -> 24 |
| `800B5628` (0-27) | default would reach Spawn's 7/23. PS2 records 18/34 are his (first motion record ID 28), both empty on GC, value 11, which no GC case yields | hook: r5 = 11, go to `800B56A4` |

Group 4 holds 1022 records in its 0x4000-byte header, one short of what two blocks need. `olkw` grows it to
0x4800 only then; group 4 is the archive's last group, so nothing else moves.
