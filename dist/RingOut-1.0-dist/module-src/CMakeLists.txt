cmake_minimum_required(VERSION 3.16)
project(recompcore_module_newabi C)

# New-ABI per-game module: head DolRecomp generated chunks + head DolRecomp's
# own cpu.c (self-consistent new CPUState: spr[1024] + mem2, no external_pointer)
# + the chassis ABI export glue.

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID")
set(GENERATED_DIR "" CACHE PATH "DolRecomp output directory")
set(DOLRECOMP_SRC "" CACHE PATH "DolRecomp src/ (provides cpu/cpu.h, common/types.h, cpu/cpu.c)")
set(GXRUNTIME_INC "" CACHE PATH "GXRuntime include/ (provides core/cpu.h for the chassis ABI header)")
set(CHASSIS_ABI_DIR "" CACHE PATH "chassis StaticRecomp dir (StaticRecompABI.h)")
set(MODULE_TEMPLATE "" CACHE PATH "module-template dir (module_export.c, gen_module_tables.py)")

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${MODULE_TEMPLATE}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${MODULE_TEMPLATE}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

# The C backend writes chunks as .c to compile; the LLVM backend writes them as
# .o to link. CMake accepts object files directly in a target's sources, so the
# only difference is which glob finds anything -- and an LLVM module additionally
# needs cpu_llvm_abi.c, because that backend CALLS the float helpers this fork's
# C backend emits inline.
file(GLOB CHUNK_SOURCES "${GENERATED_DIR}/chunks/*.c")
file(GLOB CHUNK_OBJECTS "${GENERATED_DIR}/chunks/*.o")

set(BACKEND_EXTRA_SOURCES "")
if(CHUNK_OBJECTS AND NOT CHUNK_SOURCES)
  set(BACKEND_EXTRA_SOURCES "${DOLRECOMP_SRC}/cpu/cpu_llvm_abi.c")
  set_source_files_properties(${CHUNK_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  message(STATUS "module: linking ${GENERATED_DIR}/chunks as LLVM objects")
endif()

add_library(${MODULE_NAME} SHARED
  "${MODULE_TEMPLATE}/module_export.c"
  "${MODULE_TABLES}"
  "${DOLRECOMP_SRC}/cpu/cpu.c"
  ${BACKEND_EXTRA_SOURCES}
  ${CHUNK_SOURCES}
  ${CHUNK_OBJECTS})

set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${MODULE_NAME}")
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${DOLRECOMP_SRC}"
  "${GXRUNTIME_INC}"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(${MODULE_NAME} PRIVATE m)
# Hidden visibility + no-semantic-interposition turns the hot runtime helpers
# (mem_read/write*, ppc_fp_available, resolve_addr, psq*) from PLT calls into
# direct, LTO-inlinable calls. As a default-visibility .so these were bouncing
# through the PLT on every load/store/FP guard (88k mem_read32, 87k
# ppc_fp_available, 70k mem_write32 call sites) — the dominant FMV CPU cost.
# The two chassis entry points keep explicit visibility("default").
# -flto=thin is Clang's spelling; GCC only understands plain -flto and errors
# out on "thin". setup.sh probes clang -> gcc -> cc, so a machine without clang
# hit "unrecognized argument to '-flto=' option: 'thin'" and the module build
# died. -fvisibility-inlines-hidden is also C++-only and warns on every C file.
#
# Plain `-flto` also makes GCC run the whole back end serially: it reports
# "using serial compilation of 128 LTRANS jobs" and pins one core while the
# other eleven idle. Measured on a 12-core host, a relink of this module went
# from 22 minutes to 6 with `-flto=auto`, which hands LTRANS the job server /
# core count instead. setup.sh builds this on the user's machine, so that is
# most of a coffee break per setup run.
#
# Version-guarded rather than assumed, for exactly the reason the paragraph
# above records: `auto` only exists from GCC 10 (2020), and setup.sh takes
# whatever compiler it finds.
# -DMODULE_LTO=OFF drops LTO entirely. Needed for PGO instrumentation: the
# instrumented objects are LTO IR, and mixing them with a non-LTO link silently
# produces a 21 KB stub that loads and does nothing. Not for shipping -- LTO is
# worth 5-6 minutes of link time here and was measured, not assumed.
option(MODULE_LTO "Link the module with LTO" ON)
if(NOT MODULE_LTO)
  set(MODULE_LTO_FLAG -fno-lto)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(MODULE_LTO_FLAG -flto=thin)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 10)
  set(MODULE_LTO_FLAG -flto=auto)
else()
  set(MODULE_LTO_FLAG -flto)
endif()

# ThinLTO codegen runs inside the LINKER, not the compiler, so the linker and
# its cache belong here rather than with the compile flags below.
#
# Measured on this tree, 12 cores, clang 22.1.8, 132 chunks, generated sources
# reused so only the module build is timed. ON AN IDLE MACHINE -- browser shut,
# VMs stopped -- because the first attempt at this table was taken on a busy one
# and every figure in it was wrong (see the paragraph after next):
#
#   clean, ld.bfd, no cache              144.3s mean of 3   (143.9 - 145.1)
#   clean, lld, no cache                 144.5s mean of 3   (144.0 - 145.3)
#   FRESH BUILD DIR, lld, warm cache      83.4s
#   one chunk changed, ld.bfd, no cache   61.2s / 61.5s
#   one chunk changed, lld, no cache      61.7s / 61.8s
#   one chunk changed, lld, warm cache    1.31s / 1.31s
#   nothing changed                      0.013s
#
# THE LINKER MAKES NO DIFFERENCE TO BUILD TIME: 144.3s against 144.5s over three
# alternating reps each, a 0.1% gap inside a 1% spread. Not "unresolvable" --
# measured, and zero. lld is enabled because the CACHE needs it
# (--thinlto-cache-dir is an lld option; the gold plugin spells it
# -plugin-opt=cache-dir= and was never measured), never for build speed.
#
# THE FIRST VERSION OF THIS TABLE WAS TAKEN ON A LOADED MACHINE AND EVERY ROW
# WAS INFLATED: the same clean build read 3m 04s - 3m 24s there against 144s
# here, and the same incremental read 77-80s against 61s. Worse, the noise had
# structure -- it looked like a 7% linker win, then like a 33% one, depending
# which pair you compared. If a build measurement here disagrees with this
# table by tens of seconds, suspect the machine before the code: check the load
# average and what is running, and re-measure idle rather than reasoning about
# the difference.
#
# The cache is the actual win. A clean build is 132 parallel clang invocations,
# so no linker swap can reach that part -- what the cache removes is ThinLTO
# codegen for every module that did not change, which is the whole 61 seconds of
# the link, leaving 1.31s. Note the fresh-build-dir line: even with a warm cache
# a new build tree still pays all 132 compiles, so the cache takes that case from
# 144s to 83s rather than collapsing it. ccache would cover the other half.
#
# The 1.4s is a lower bound, honestly labelled: the test edit was dead-stripped,
# so every module still cache-hit and only the chunk recompile (~1.2s) and a
# fully-cached link (~0.15s) were measured. A chunk whose code really changes
# adds codegen for that one module, which the 77s/132 rate puts near 0.6s. Call
# a real one-chunk edit ~2s rather than ~1.4s. Either way it is ~40x, not the
# 420x that comparing against a no-op build would suggest.
#
# setup.sh does `rm -rf work/` every run, so this only pays off on a re-run --
# which setup.sh explicitly invites ("worth re-running after you have played",
# to pick up the memory-card icon). That re-run currently rebuilds a module that
# is byte-for-byte what it already had.
option(MODULE_LLD "Link with lld when it is usable; ThinLTO links far faster" ON)
set(MODULE_LINK_CACHE "" CACHE STRING
    "ThinLTO cache directory; empty picks the XDG default, NONE disables it")

# PROBED, never assumed. lld is a separate package from clang on Arch, Debian
# and Fedora alike, and setup.sh takes whatever compiler it finds (clang -> gcc
# -> cc). This file already has one scar from assuming a clang spelling was
# present -- see the -flto=thin note above, which killed the module build on
# every machine without it. A player who has clang but not lld must get the
# ordinary linker, not a failed setup.
if(MODULE_LTO AND MODULE_LLD AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND UNIX AND NOT APPLE)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld)
  check_c_source_compiles("int main(void){return 0;}" MODULE_LLD_USABLE)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

if(MODULE_LLD AND MODULE_LLD_USABLE)
  target_link_options(${MODULE_NAME} PRIVATE -fuse-ld=lld)

  # NOT under module-src/. package-dist.sh copies this whole tree into the
  # release stage with `cp -a`, so a cache next to these sources would ship 42 MB
  # of one developer's build artefacts inside the zip, and would sit untracked in
  # git besides. It has to outlive `rm -rf work/`, which is the one thing work/
  # cannot do, so it goes to the user's XDG cache.
  if(NOT MODULE_LINK_CACHE)
    if(DEFINED ENV{XDG_CACHE_HOME})
      set(MODULE_LINK_CACHE "$ENV{XDG_CACHE_HOME}/ringout/thinlto")
    elseif(DEFINED ENV{HOME})
      set(MODULE_LINK_CACHE "$ENV{HOME}/.cache/ringout/thinlto")
    endif()
  endif()

  # Bounded on purpose: one clean build leaves 135 entries / 42 MB, and every
  # rebuilt variant adds more. lld's default policy prunes on a 20-minute
  # interval and will otherwise let the directory grow to half the free space on
  # the disk, which is not a reasonable thing to leave on a player's machine.
  if(MODULE_LINK_CACHE AND NOT MODULE_LINK_CACHE STREQUAL "NONE")
    file(MAKE_DIRECTORY "${MODULE_LINK_CACHE}")
    target_link_options(${MODULE_NAME} PRIVATE
      "-Wl,--thinlto-cache-dir=${MODULE_LINK_CACHE}"
      "-Wl,--thinlto-cache-policy=cache_size_bytes=2g:prune_after=168h")
    message(STATUS "module: ThinLTO cache at ${MODULE_LINK_CACHE}")
  endif()
elseif(MODULE_LLD AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  # Say plainly that this is FINE. CMake prints the probe above as
  # "Performing Test MODULE_LLD_USABLE - Failed", which reads like a broken
  # build to anyone who has not read this file -- and it has been reported as
  # one. lld is a separate package from clang on Arch, Debian and Fedora, so a
  # stock Steam Deck hits this every time.
  #
  # It costs NOTHING, measured, not assumed: 144.3s with ld.bfd against 144.5s
  # with lld over three alternating reps each, a 0.1% gap inside a 1% spread
  # (the table further up). lld is on for the ThinLTO CACHE, which is an lld
  # option -- and setup.sh does `rm -rf work/` every run, so that cache cannot
  # help a setup.sh build anyway.
  message(STATUS "module: lld is not installed -- linking with the default "
                 "linker instead.")
  message(STATUS "        This is FINE and costs nothing: measured at 144.3s "
                 "vs 144.5s.")
  message(STATUS "        The 'MODULE_LLD_USABLE - Failed' line above is that "
                 "check, not an error.")
endif()

# Profile-guided optimisation. Point MODULE_PGO_PROFILE at a clang .profdata and
# the chunks are compiled with -fprofile-use.
#
# Measured 2026-08-12, 24000 frames of arcade-match.txt, 5 reps per arm, idle
# machine, profile trained on gameplay (NOT boot/menus -- menus carry ~0.1% of a
# session's paired-single traffic, so a profile taken there optimises code the
# game barely runs):
#
#   base   889.39 Gcyc   2185.7 Ginsn   IPC 2.456
#   pgo    783.57 Gcyc   1970.0 Ginsn   IPC 2.514
#
# -11.9% cycles, non-overlapping across reps, and windowed on a real match
# 54.5 -> 59.6 fps. The mechanism is -9.9% RETIRED INSTRUCTIONS plus 2.4% IPC:
# fewer host instructions per guest instruction, which is the one lever the PMU
# work said was left and which no amount of per-opcode inspection could find --
# the reduction is diffuse, not concentrated in any hot opcode class.
#
# Guest state is unaffected: all 10 benchmark runs hashed 41f50d828b60, so a PGO
# module and a plain one are netplay peers. This is codegen only.
#
# Costs, so nobody is surprised: .text 20.69 -> 37.47 MB and the module build
# 144s -> 446s. setup.sh builds on the player's machine, so that is the trade --
# three times the wait, once, for ~10% of the frame budget back.
#
# GCC is excluded deliberately: it cannot read a clang profile, and GCC's own PGO
# is the path that died with SIGBUS during boot (see the notes on -DMODULE_LTO=OFF
# above). PROBED rather than assumed, like lld: a profile written by a newer
# clang than the player's is rejected by -fprofile-use, and setup.sh takes
# whatever compiler it finds. An unusable profile means "build normally", never a
# failed setup.
set(MODULE_PGO_PROFILE "" CACHE FILEPATH
    "clang .profdata to compile the chunks with (-fprofile-use); empty disables PGO")

if(MODULE_PGO_PROFILE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  if(NOT EXISTS "${MODULE_PGO_PROFILE}")
    message(STATUS "module: PGO profile not found, building without it: ${MODULE_PGO_PROFILE}")
  else()
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fprofile-use=${MODULE_PGO_PROFILE}")
    check_c_source_compiles("int main(void){return 0;}" MODULE_PGO_USABLE)
    unset(CMAKE_REQUIRED_FLAGS)

    # A profile can be structurally VALID and still say every block is cold.
    # A collection run that never reached gameplay -- or was not headless when
    # it needed to be -- writes a file that -fprofile-use accepts happily and
    # that tells the optimiser nothing is hot. The result is a "PGO" build that
    # is WORSE than an unprofiled one, and the only warnings that would say so
    # (-Wprofile-instr-unprofiled) are suppressed below because unprofiled
    # non-chunk code is expected here. So check the counts directly.
    #
    # Falls back to building WITH the profile if llvm-profdata is missing: the
    # player's machine is not required to have LLVM's tools, and a missing tool
    # is not evidence of a bad profile. Same philosophy as the lld and compiler
    # probes -- degrade, never hard-fail a player's build.
    if(MODULE_PGO_USABLE)
      get_filename_component(_pgo_bindir "${CMAKE_C_COMPILER}" DIRECTORY)
      find_program(MODULE_LLVM_PROFDATA NAMES llvm-profdata HINTS "${_pgo_bindir}")
      if(MODULE_LLVM_PROFDATA)
        execute_process(COMMAND "${MODULE_LLVM_PROFDATA}" show "${MODULE_PGO_PROFILE}"
                        OUTPUT_VARIABLE _pgo_show ERROR_QUIET RESULT_VARIABLE _pgo_rc)
        if(_pgo_rc EQUAL 0 AND _pgo_show MATCHES "Total count: ([0-9]+)")
          if(CMAKE_MATCH_1 STREQUAL "0")
            message(WARNING "module: PGO profile has a total count of ZERO -- the "
                            "collection run recorded nothing. Building WITHOUT it, "
                            "because a cold profile is worse than none.")
            set(MODULE_PGO_USABLE OFF)
          else()
            message(STATUS "module: PGO profile total count ${CMAKE_MATCH_1}")
          endif()
        endif()
      endif()
    endif()

    if(MODULE_PGO_USABLE)
      # The warnings are expected, not a problem: the profile covers the chunks
      # and the runtime helpers, and anything else in the build is legitimately
      # unprofiled.
      #
      # -Wno-backend-plugin is the third of these and the one that matters for a
      # disc that is not the US one. The profile is trained on GRSEAF, and chunk
      # functions are named for their guest address, so on GRSPAF -- whose text1
      # starts at the same address -- the NAMES match while the control flow
      # inside does not. LLVM discards those counts and says so per chunk:
      #
      #   function control flow change detected (hash mismatch)
      #   func_80205940 Hash = ... count discarded [-Wbackend-plugin]
      #
      # Nine of those in a PAL build, scattered through an otherwise clean
      # compile, which reads as something being wrong when nothing is. There is
      # no narrower group for it -- LLVM emits every backend diagnostic through
      # -Wbackend-plugin -- so this silences the group and setup.sh states the
      # situation once instead, in a sentence a player can act on.
      target_compile_options(${MODULE_NAME} PRIVATE
        "-fprofile-use=${MODULE_PGO_PROFILE}"
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
        -Wno-backend-plugin)
      message(STATUS "module: PGO enabled from ${MODULE_PGO_PROFILE}")
    else()
      # Almost always a FORMAT VERSION problem, not a corrupt file: an indexed
      # profile can only be read by an LLVM at least as new as the one that
      # wrote it. These ship generated by LLVM 22, so clang 20 (SteamOS 3.8,
      # Fedora), 18 (Ubuntu 24.04) and 14 (Debian 12) all refuse them with
      # "unsupported instrumentation profile format version".
      #
      # The build is correct either way -- this costs 10-14% of CPU time
      # (10.3% desktop, 13.0% Deck, over 6000 fixed frames of an arcade match)
      # -- so say what it costs and what would fix it, rather than just that
      # something was unusable.
      message(STATUS "module: this clang cannot read the shipped PGO profile")
      message(STATUS "        (it was generated by LLVM 22; older clang cannot read that format)")
      message(STATUS "        Building without it. The module is correct, 10-14% slower.")
      message(STATUS "        Two ways to get it back: clang 22 or newer will read")
      message(STATUS "        this file as it is, or ./setup.sh --pgo will train a new")
      message(STATUS "        profile with the clang you already have (~30 min, once).")
    endif()
  endif()
endif()

# Line-level profiling (-DMODULE_DEBUG_LINES=ON). Each chunk is ONE huge C
# function, so perf can only ever report "func_8000D940" -- a 16 KB span holding
# dozens of guest functions. That is enough to see which chunk is hot and no more.
#
# -g1 emits line tables only, so `perf report -s srcline` resolves samples to a
# line of the generated .c; the emitter writes one `label_<guest addr>:` per guest
# instruction, so that line maps back to a guest PC.
# .github/scripts/hot-guest-code.sh does the mapping and prints hot guest
# addresses.
#
# Off by default: it roughly doubles the .so and there is no reason to ship it.
# Codegen is unaffected -- the .text bytes are identical with and without it --
# so a -g1 build stays timing-comparable with a normal one.
option(MODULE_DEBUG_LINES "Emit line tables so perf can attribute inside chunks" OFF)
if(MODULE_DEBUG_LINES)
  target_compile_options(${MODULE_NAME} PRIVATE -g1)
  target_link_options(${MODULE_NAME} PRIVATE -g1)
endif()

# -ffp-contract=off is NOT a tuning choice, it is what makes this module
# reproducible across machines, and netplay depends on that.
#
# GCC defaults to -ffp-contract=fast, so it may fuse an incidental a*b+c in the
# emitted C into an FMA -- and whether it does depends on -march. Measured: a
# module built -march=x86-64-v3 and one built -march=znver2 diverge in guest RAM
# at FRAME 176. setup.sh builds with -march=native, so two desktop players on
# different CPUs get modules that cannot stay in sync, and a Deck cannot play a
# PC. With this flag the same two builds are byte-identical over 600 frames.
#
# It costs nothing measurable (7.20s vs 7.33s on a 600-frame run, i.e. inside
# the noise and if anything faster): the guest's own FMA instructions are
# emitted as explicit ppc_fma() calls, so auto-contraction was only ever
# catching incidental arithmetic.
#
# It covers the LINKER too, which had to be checked rather than assumed. Because
# ThinLTO codegen happens inside the linker, an lld-linked module's .text differs
# from the ld.bfd one -- same 20,689,042 bytes, different contents -- and
# "different code that should behave identically" is exactly what FRAME 176 above
# disproved once. Measured 2026-08-12: two modules built from identical sources,
# one per linker, give byte-identical per-frame RAM hashes over 24000 frames of
# arcade-match.txt (boot -> Arcade -> two full matches with active input), the
# same standard the compiler-version and -march comparisons were held to.
# -DMODULE_LLD=OFF restores the old linker if a future parity run points here.
#
# Identical guest state says nothing about how many CYCLES it takes to reach it,
# so that was measured separately (module-bench.sh, 24000 frames of
# arcade-match.txt, 5 runs per arm, alternating order, idle machine):
#
#   ld.bfd  877.95 Gcyc mean   877.55 - 878.83   spread 0.15%
#   lld     873.35 Gcyc mean   872.18 - 874.41   spread 0.26%   (+ one outlier)
#
# lld is ~0.5% CHEAPER, and on four of five reps every lld run sits below every
# ld.bfd run with no overlap. The fifth lld rep came in at 884.78 with IPC 2.470
# against 2.500-2.507 for its siblings and the longest wall of the ten, which is
# a contention signature, not codegen. Either way the effect is half a percent:
# real, repeatable, and far too small to choose a linker over. Instruction counts
# agree to 0.03% and all runs hash identically, so both arms did the same work.
#
# THE 8% THAT WAS NOT THERE: an earlier session on a loaded machine read "ld.bfd
# is 8% cheaper" from three adjacent-run pairs. It was an artifact of run order
# under contention -- per-arm spread was 12-16% there against 0.15% here, and
# replication flipped the sign. A BOLT-sized regression that would have flipped
# this default turned out to be the browser. When a codegen A/B on this host
# shows anything in the single-digit percents, close everything and re-run before
# believing it.
# A guard against a leak that does not currently exist, kept small and honest.
#
# MEASURED: adding this changes nothing today. A module built with it is
# BYTE-IDENTICAL to one built without, and neither contains a single builder
# path -- unlike the runtime, which had 440 __FILE__ strings in .rodata and
# needs its own copy of this flag. Nothing in module-src asserts or logs a
# path. It stays as insurance for the day something here does.
#
# It was added believing it would ALSO make the PGO profile portable. It does
# not. LLVM keys an internal-linkage function "<source path>;<symbol>" from the
# filename as passed to the compiler, and this flag's documented scope is debug
# info, coverage mapping, preprocessor macros and __builtin_FILE(); there is no
# -fprofile-prefix-map. Confirmed twice: the flag was present in build.ninja and
# the retrained profile still carried the absolute path, and compiling one file
# both ways gives "sub/t.c;helper" relative against the full path absolute.
#
# So chassis_dispatch, chassis_on_state_loaded and resolve_addr carry the
# TRAINING machine's path in their key and get no profile data on anyone else's
# machine. The chunk functions are external and unaffected, which is why PGO is
# still worth 10-14%. The real fix is external linkage for those three, which is
# a codegen change wanting a measurement, not a flag.
set(MODULE_PATH_MAP)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(MODULE_PATH_MAP -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/ringout)
endif()

target_compile_options(${MODULE_NAME} PRIVATE
  ${MODULE_LTO_FLAG} -fvisibility=hidden -fno-semantic-interposition
  -ffp-contract=off ${MODULE_PATH_MAP})
target_link_options(${MODULE_NAME} PRIVATE ${MODULE_LTO_FLAG})
