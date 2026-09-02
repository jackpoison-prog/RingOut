// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompMods.h"

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/RecompGameData.h"
#include "VideoCommon/RecompOlk.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace RecompMods
{
namespace
{
constexpr char kSection[] = "Mods";

std::mutex s_mutex;
std::atomic<std::uint32_t> s_generation{0};

std::string ModsRoot()
{
  return File::GetUserPath(D_LOAD_IDX) + "Mods" DIR_SEP;
}

std::string IniPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "Mods.ini";
}

// A mod is off unless the ini says otherwise. Dropping a folder in should not
// silently change what the game looks like -- the player turns it on in the
// menu, which is also the moment they find out whether it is netplay-safe.
// Extractors in order of how likely they are to be on the machine already.
// bsdtar leads because it is libarchive's CLI and libarchive is a dependency of
// pacman itself, so SteamOS has it without the player installing anything; the
// Debian build container has none of these, which is why this is a runtime
// lookup and not a link-time dependency. Nothing is bundled: shipping a RAR
// implementation for a feature most players never touch is not a trade worth
// making, and every one of these reads the RAR5 files GameBanana serves.
struct Extractor
{
  const char* tool;
  // %A archive, %D destination -- substituted, never shell-interpolated.
  std::vector<const char*> args;
};

const std::vector<Extractor>& Extractors()
{
  static const std::vector<Extractor> list = {
      {"bsdtar", {"-xf", "%A", "-C", "%D"}},
      {"unrar", {"x", "-y", "%A", "%D"}},
      {"7z", {"x", "-y", "-o%D", "%A"}},
      {"unar", {"-q", "-f", "-o", "%D", "%A"}},
  };
  return list;
}

bool ToolExists(const std::string& tool)
{
  for (const char* dir : {"/usr/bin/", "/bin/", "/usr/local/bin/"})
  {
    if (File::Exists(dir + tool))
      return true;
  }
  return false;
}

// fork/exec rather than system(): a mod folder is named by whatever the player
// downloaded, and passing that through a shell is how a filename with a quote
// or a semicolon in it becomes a command. argv entries are never parsed.
bool RunExtractor(const Extractor& ex, const std::string& archive, const std::string& dest)
{
  std::vector<std::string> argv_storage = {ex.tool};
  for (const char* a : ex.args)
  {
    std::string arg = a;
    const auto sub = [&](const char* token, const std::string& value) {
      const auto pos = arg.find(token);
      if (pos != std::string::npos)
        arg.replace(pos, 2, value);
    };
    sub("%A", archive);
    sub("%D", dest);
    argv_storage.push_back(std::move(arg));
  }

  std::vector<char*> argv;
  for (auto& a : argv_storage)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0)
  {
    // The child's output would otherwise land in the middle of the game's log.
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0)
    {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    execvp(ex.tool, argv.data());
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool IsArchive(const std::string& ext)
{
  return ext == ".rar" || ext == ".zip" || ext == ".7z";
}

// Unpack Load/Mods/<name>.rar into Load/Mods/<name>/.
//
// Only when the destination does not already exist: re-extracting on every scan
// would undo any edit the player made to the unpacked files, and would do it
// silently. The archive is left in place afterwards rather than deleted -- it is
// the player's download, and deleting it to save a few MB is not this code's
// call to make.
void ExtractArchives()
{
  std::error_code ec;
  const std::filesystem::path root(ModsRoot());
  std::vector<std::filesystem::path> archives;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec))
  {
    if (!entry.is_regular_file(ec))
      continue;
    std::string ext = entry.path().extension().string();
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (IsArchive(ext))
      archives.push_back(entry.path());
  }

  for (const auto& archive : archives)
  {
    const auto dest = root / archive.stem();
    if (std::filesystem::exists(dest, ec))
      continue;

    std::filesystem::create_directories(dest, ec);
    bool ok = false;
    for (const auto& ex : Extractors())
    {
      if (!ToolExists(ex.tool))
        continue;
      if (RunExtractor(ex, archive.string(), dest.string()))
      {
        ok = true;
        INFO_LOG_FMT(VIDEO, "[mods] unpacked {} with {}", archive.filename().string(), ex.tool);
        break;
      }
    }
    if (!ok)
    {
      // Leave the empty folder: it becomes the menu row that explains why the
      // archive did not turn into a mod, which is more use than the download
      // vanishing without comment.
      WARN_LOG_FMT(VIDEO, "[mods] could not unpack {} -- no working extractor",
                   archive.filename().string());
    }
  }
}

// What did the archive actually contain? Decided by extension, one level deep,
// because mods are commonly packed with a folder inside the archive.
Kind ClassifyFolder(const std::filesystem::path& folder, std::string* note,
                    std::string* payload)
{
  std::error_code ec;
  bool textures = false, game_data = false, empty = true;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec))
  {
    if (!entry.is_regular_file(ec))
      continue;
    empty = false;
    std::string ext = entry.path().extension().string();
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".png" || ext == ".dds")
      textures = true;
    else if (ext == ".dtp")
    {
      game_data = true;
      if (payload->empty())
        *payload = entry.path().string();
    }
  }

  if (textures)
    return Kind::Textures;
  if (game_data)
    return Kind::GameData;
  *note = empty ? "nothing unpacked - install bsdtar, or unpack it yourself"
                : "no textures found in this folder";
  return Kind::Unknown;
}

bool ReadEnabled(Common::IniFile& ini, const std::string& name)
{
  bool enabled = false;
  ini.GetIfExists(kSection, name, &enabled);
  return enabled;
}
}  // namespace

std::vector<Mod> Scan()
{
  std::lock_guard lock(s_mutex);

  ExtractArchives();

  Common::IniFile ini;
  ini.Load(IniPath());

  std::vector<Mod> mods;
  std::error_code ec;
  const std::filesystem::path root(ModsRoot());
  for (const auto& entry : std::filesystem::directory_iterator(root, ec))
  {
    if (!entry.is_directory(ec))
      continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || name.front() == '.')
      continue;
    Mod mod;
    mod.name = name;
    mod.path = entry.path().string();
    mod.enabled = ReadEnabled(ini, name);
    mod.kind = ClassifyFolder(entry.path(), &mod.note, &mod.payload);

    if (mod.kind == Kind::GameData)
    {
      const std::string olk = RecompGameData::RootOlkPath();
      std::error_code size_ec;
      const auto payload_size = std::filesystem::file_size(mod.payload, size_ec);
      const auto slot = olk.empty() || size_ec
                            ? RecompOlk::Slot{}
                            : RecompOlk::FindUniqueSlotBySize(
                                  olk, static_cast<std::uint32_t>(payload_size));
      if (slot.found)
      {
        mod.installable = true;
        mod.slot_offset = slot.offset;
        mod.slot_size = slot.size;
        mod.installed = RecompOlk::SlotMatchesFile(olk, slot, mod.payload);
      }
      else
      {
        // No slot of that exact size, or more than one. Either way this cannot
        // be dropped in without rebuilding the container.
        mod.note = "skin needs olkviewer - no single slot of its size";
      }
    }

    mods.push_back(std::move(mod));
  }

  // Sorted so the menu row order is stable between launches, and so precedence
  // between two enabled mods is at least predictable rather than filesystem
  // order. Two mods replacing the same texture is the player's problem to
  // resolve; the earlier name wins and the menu says so.
  std::ranges::sort(mods, {}, &Mod::name);
  return mods;
}

std::vector<std::string> EnabledDirectories()
{
  std::vector<std::string> dirs;
  for (const auto& mod : Scan())
  {
    // A GameData mod has no textures to layer and an Unknown one has nothing
    // recognisable; handing either to the texture loader would just make it
    // walk a folder that cannot contribute.
    if (mod.enabled && mod.kind == Kind::Textures)
      dirs.push_back(mod.path);
  }
  return dirs;
}

bool RequestInstall(const Mod& mod, std::string* message)
{
  if (!mod.installable)
  {
    if (message)
      *message = "This skin has no matching slot - use olkviewer.";
    return false;
  }
  if (mod.installed)
  {
    if (message)
      *message = "Already installed. Use Restore original game data to remove it.";
    return false;
  }

  const std::string path = File::GetUserPath(D_USER_IDX) + "mod-install.request";
  std::ofstream file(path, std::ios::app);
  if (!file)
  {
    if (message)
      *message = "Could not write the install request.";
    return false;
  }
  file << mod.slot_offset << ' ' << mod.slot_size << ' ' << mod.payload << '\n';
  if (message)
    *message = mod.name + " will be written into the game on the next launch.";
  return true;
}

void SetEnabled(const std::string& name, bool enabled)
{
  std::lock_guard lock(s_mutex);

  Common::IniFile ini;
  ini.Load(IniPath());
  ini.GetOrCreateSection(kSection)->Set(name, enabled);
  ini.Save(IniPath());
  ++s_generation;
}

std::uint32_t Generation()
{
  return s_generation.load();
}

bool IsEnabled(const std::string& name)
{
  std::lock_guard lock(s_mutex);

  Common::IniFile ini;
  ini.Load(IniPath());
  return ReadEnabled(ini, name);
}
}  // namespace RecompMods
