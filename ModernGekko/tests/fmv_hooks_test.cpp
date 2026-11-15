// The per-disc FMV hook table (StaticRecompFmvHooks.h).
//
// Beyond the lookup itself, this checks the table against setup.sh AND
// setup.ps1: every disc whose row the runtime can use must pass exactly its six
// hook PCs as --dispatch-pc (and its two render-layer hooks as --gated-pc), or --direct-calls jumps straight past them and the
// hook never fires. The three lists live in three languages, so nothing else
// keeps them in step.

#include "StaticRecompFmvHooks.h"
#include "StaticRecompRenderLayerHook.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

std::set<std::uint32_t> AsSet(const StaticRecompFmvHookPcs& p) {
  return {p.start_afs, p.cnv_frm, p.exec_svr, p.next_frm_ready, p.rel_cur_frm, p.getfrm};
}

bool AllUnset(const StaticRecompFmvHookPcs& p) {
  return AsSet(p) == std::set<std::uint32_t>{kStaticRecompNoFmvHook};
}

std::string ReadFile(const char* path) {
  std::ifstream in(path);
  Check(static_cast<bool>(in), path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The values of `flag` (--dispatch-pc or --gated-pc) in the case arm of
// setup.sh that names disc_id.
std::set<std::uint32_t> SetupPcs(const std::string& text, const std::string& disc_id,
                                 const std::string& flag) {
  std::set<std::uint32_t> out;
  // Arms look like `GRSEAF|GRSEPS) LEADER_CASES+=(--direct-calls ... );;`.
  static const std::regex arm(R"(\n([A-Z0-9|]+)\) LEADER_CASES\+=\(--direct-calls([^;]*)\);;)");
  const std::regex pc(flag + R"( (0x[0-9A-Fa-f]+))");
  for (std::sregex_iterator it(text.begin(), text.end(), arm), end; it != end; ++it) {
    const std::string ids = "|" + (*it)[1].str() + "|";
    if (ids.find("|" + disc_id + "|") == std::string::npos)
      continue;
    const std::string body = (*it)[2].str();
    for (std::sregex_iterator p(body.begin(), body.end(), pc); p != end; ++p)
      out.insert(static_cast<std::uint32_t>(std::stoul((*p)[1].str(), nullptr, 16)));
  }
  return out;
}

// The same for setup.ps1, whose blocks look like
// `elseif ($DiscId -eq 'GRSPAF') {\n ... '--dispatch-pc', '0x...', ... \n}`.
std::set<std::uint32_t> SetupPs1Pcs(const std::string& text, const std::string& disc_id,
                                    const std::string& flag) {
  std::set<std::uint32_t> out;
  static const std::regex block(R"((?:if|elseif) \(([^\n{]*)\) \{\n([\s\S]*?)\n\})");
  const std::regex pc("'" + flag + R"(', '(0x[0-9A-Fa-f]+)')");
  for (std::sregex_iterator it(text.begin(), text.end(), block), end; it != end; ++it) {
    if ((*it)[1].str().find("'" + disc_id + "'") == std::string::npos)
      continue;
    const std::string body = (*it)[2].str();
    for (std::sregex_iterator p(body.begin(), body.end(), pc); p != end; ++p)
      out.insert(static_cast<std::uint32_t>(std::stoul((*p)[1].str(), nullptr, 16)));
  }
  return out;
}

}  // namespace

int main() {
  const auto us = StaticRecompFindFmvHooks("GRSEAF", false);
  Check(us.start_afs == 0x8020C1E8u, "US mwPlyStartAfs");
  Check(us.cnv_frm == 0x80209138u, "US mwPlyFxCnvFrmARGB");
  Check(us.getfrm == 0x80208244u, "US getfrm");
  Check(AsSet(StaticRecompFindFmvHooks("GRSEPS", false)) == AsSet(us), "Plus matches US");

  // PAL is mapped and playtested, so it is on without the opt-in.
  const auto pal = StaticRecompFindFmvHooks("GRSPAF", false);
  Check(pal.start_afs == 0x80213938u, "PAL mwPlyStartAfs");
  // The opt-in gate itself: every row not yet playtested stays off without it.
  for (const auto& row : kStaticRecompFmvHookTable)
    if (!row.playtested)
      Check(AllUnset(StaticRecompFindFmvHooks(row.game_id, false)), "unplaytested row off by default");
  for (const auto& row : kStaticRecompFmvHookTable)
    if (std::string(row.game_id) == "GRSPAF")
      Check(pal.start_afs - us.start_afs == 0x7750u && pal.cnv_frm - us.cnv_frm == 0x7750u &&
                pal.exec_svr - us.exec_svr == 0x7750u &&
                pal.next_frm_ready - us.next_frm_ready == 0x7750u &&
                pal.rel_cur_frm - us.rel_cur_frm == 0x7750u && pal.getfrm - us.getfrm == 0x7750u,
            "PAL is the US library at +0x7750");

  // JP is unmapped, and an unknown disc gets nothing, opted in or not.
  Check(AllUnset(StaticRecompFindFmvHooks("GRSJAF", true)), "JP has no hooks");
  Check(AllUnset(StaticRecompFindFmvHooks("GXXE01", true)), "unknown disc has no hooks");
  Check(AllUnset(StaticRecompFindFmvHooks(nullptr, true)), "null game id has no hooks");

  const std::string setup = ReadFile(RINGOUT_SETUP_SH);
  const std::string setup_ps1 = ReadFile(RINGOUT_SETUP_PS1);
  // The scene-effect hooks (StaticRecompRenderLayerHook.h) are passed as
  // --gated-pc, beside the movie player's six --dispatch-pc, on the discs with
  // screen shapes: native until the run loop arms the gate.
  Check(StaticRecompFindRenderLayerHooks("GRSEAF").layer_pc == 0x80020348u, "US layer hook");
  Check(StaticRecompFindRenderLayerHooks("GRSEAF").item_pc == 0x8001F804u, "US item hook");
  Check(StaticRecompFindRenderLayerHooks("GRSEPS").item_pc == 0x8001F804u, "Plus item hook");
  Check(StaticRecompFindRenderLayerHooks("GRSPAF").item_pc == 0x8001F9D8u, "PAL item hook");
  Check(StaticRecompFindRenderLayerHooks("GRSJAF").layer_pc == kStaticRecompNoLayerHook,
        "JP has no layer hook");
  Check(StaticRecompIsSceneEffect(false, 2, 1, 2), "layer-2 effect sprite");
  Check(!StaticRecompIsSceneEffect(false, 6, 1, 2), "HUD layer is never an effect");
  Check(StaticRecompIsSceneEffect(true, 6, 1, 6), "per-view drawer after the HUD layer");
  Check(!StaticRecompIsSceneEffect(false, 2, 3, 2), "three-stage menu panel");
  Check(!StaticRecompIsSceneEffect(false, 2, 1, 1), "setup-1 menu panel");
  for (const auto& row : kStaticRecompFmvHookTable) {
    const auto want = AsSet(row.pcs);
    std::set<std::uint32_t> want_gated;
    const StaticRecompRenderLayerHooks layer = StaticRecompFindRenderLayerHooks(row.game_id);
    if (layer.layer_pc != kStaticRecompNoLayerHook)
      want_gated = {layer.layer_pc, layer.item_pc};
    if (SetupPcs(setup, row.game_id, "--dispatch-pc") != want ||
        SetupPcs(setup, row.game_id, "--gated-pc") != want_gated) {
      std::fprintf(stderr, "FAIL: setup.sh's --dispatch-pc / --gated-pc lists for %s are not its hooks\n",
                   row.game_id);
      ++g_failures;
    }
    if (SetupPs1Pcs(setup_ps1, row.game_id, "--dispatch-pc") != want ||
        SetupPs1Pcs(setup_ps1, row.game_id, "--gated-pc") != want_gated) {
      std::fprintf(stderr, "FAIL: setup.ps1's --dispatch-pc / --gated-pc lists for %s are not its hooks\n",
                   row.game_id);
      ++g_failures;
    }
  }
  // A disc with no row must not pass any: they would only make unrelated code
  // dispatcher-reachable.
  Check(SetupPcs(setup, "GRSJAF", "--dispatch-pc").empty() && SetupPcs(setup, "GRSJAF", "--gated-pc").empty(),
        "JP passes no hook PCs (setup.sh)");
  Check(SetupPs1Pcs(setup_ps1, "GRSJAF", "--dispatch-pc").empty() &&
            SetupPs1Pcs(setup_ps1, "GRSJAF", "--gated-pc").empty(),
        "JP passes no hook PCs (setup.ps1)");

  if (g_failures == 0)
    std::puts("fmv_hooks: ok");
  return g_failures == 0 ? 0 : 1;
}
