// Guest RAM through Dolphin's memory arena, on whatever page size the host has.
//
// Apple Silicon pages are 16 KB where every x86 machine here has 4 KB, and the
// arena maps GameCube RAM as views of one shared segment at fixed offsets. A
// 4 KB assumption anywhere in that path is a crash at boot on a Mac, and qemu
// on a Linux desktop cannot show it (qemu-user runs on the host's 4 KB pages),
// so this runs in the macOS CI job on real hardware. It runs everywhere else
// too, and prints the page size so a log says which case it covered.

#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include <cstdio>
#include <unistd.h>

int main()
{
  std::printf("page size: %ld\n", sysconf(_SC_PAGESIZE));

  auto& memory = Core::System::GetInstance().GetMemory();
  memory.Init();
  if (memory.GetRAM() == nullptr)
    return 1;
  if (!memory.InitFastmemArena())
    return 2;

  // First word, last word of main RAM, through the cached and uncached
  // mirrors: the two views of one segment must alias.
  memory.Write_U32(0x12345678u, 0x80000100u);
  if (memory.Read_U32(0xC0000100u) != 0x12345678u)
    return 3;
  memory.Write_U32(0xCAFEF00Du, 0x817FFFFCu);
  if (memory.Read_U32(0xC17FFFFCu) != 0xCAFEF00Du)
    return 4;
  // The fastmem view sees the same bytes (big-endian in guest order).
  const u8* phys = memory.GetPhysicalBase();
  if (phys == nullptr || phys[0x100] != 0x12 || phys[0x103] != 0x78)
    return 5;

  memory.ShutdownFastmemArena();
  memory.Shutdown();
  return 0;
}
