// Copyright 2026 RingOut contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "Common/CommonTypes.h"

// Whether the game is drawing a scene effect, carried from the CPU thread to the
// GPU in FIFO order WITHOUT touching guest memory (StaticRecompRenderLayerHook.h
// says what a scene effect is and why the GPU wants to know).
//
// The obvious way -- writing a BP command into the gather pipe -- works, and
// changes the game: the bytes land in the guest's FIFO buffer, every pointer
// after them moves, and by frame 3000 of the arcade route 585 KB of RAM differed
// from a run without them (21:9 vs 16:9 alone differs by 4 KB, the wide-mode
// constants). A 21:9 player would then play a different match from a 16:9 one,
// which netplay and replays cannot have.
//
// So the CPU only records WHERE in the FIFO the marker belongs: the 32-byte
// block the next gather-pipe burst lands in, the byte offset inside it, and the
// bytes the game has already put in front of it. The GPU splices the command
// into its own copy of that block (the video buffer, host memory) as it reads
// it, so the decoder meets it exactly where the game's commands put it, and acts
// on it at no cost in GPU cycles, which feed guest-visible timing in the
// synchronous modes (OpcodeDecoding.cpp). With the hook on, the RAM hash matched
// a run without it over 4000 frames of the arcade route.
//
// One producer (the CPU thread), one consumer (whichever thread reads the FIFO;
// they never overlap).
namespace RecompLayerMarkers
{
inline constexpr u8 kBpOpcode = 0x61;
// Not a hardware register; the decoder intercepts it before LoadBPReg.
inline constexpr u8 kLayerBpReg = 0x7E;
inline constexpr size_t kCommandSize = 5;
// At most this many markers are spliced into one block, so the video buffer
// needs at most this much room beyond the block itself.
inline constexpr size_t kMaxPerBlock = 4;
inline constexpr size_t kMaxExtraBytes = kMaxPerBlock * kCommandSize;

struct Marker
{
  u32 block;
  u32 offset;
  std::atomic<u32> value;
  // The block's first `offset` bytes as the game wrote them, and the whole block
  // in front of it. The FIFO is a ring (two, in this game, used on alternate
  // frames), so an address alone does not say which pass through it a marker
  // belongs to, and a short prefix -- none, at offset 0 -- cannot either: one at
  // offset 0 once matched a later pass and broke the stream.
  std::array<u8, 32> prefix;
  u32 prev_block;
  std::array<u8, 32> before;
};

struct Queue
{
  static constexpr u32 kSize = 256;
  std::array<Marker, kSize> ring{};
  std::atomic<u32> head{0};  // next to consume (consumer-owned)
  std::atomic<u32> tail{0};  // next to fill (producer-owned)
};

inline Queue g_queue;

// Consumer side: the block read before the current one, as the game wrote it.
inline u32 g_last_block = 0xFFFFFFFFu;
inline std::array<u8, 32> g_last_bytes{};

// CPU thread: the next FIFO byte the game writes is at `offset` in the block at
// `block`, preceded in that block by `prefix` and in the FIFO by the 32 bytes
// `before` at `prev_block`; from there on the value is `value` (24 bits).
inline void Push(u32 block, u32 offset, u32 value, const u8* prefix, u32 prev_block,
                 const u8* before)
{
  Queue& q = g_queue;
  const u32 tail = q.tail.load(std::memory_order_relaxed);
  // Back-to-back calls with nothing drawn between them share a position, and
  // only the last one matters. Its block has not been written yet, so the
  // consumer cannot be looking at it.
  if (tail != q.head.load(std::memory_order_acquire))
  {
    Marker& last = q.ring[(tail - 1) % Queue::kSize];
    if (last.block == block && last.offset == offset)
    {
      last.value.store(value, std::memory_order_release);
      return;
    }
  }
  if (tail - q.head.load(std::memory_order_acquire) >= Queue::kSize)
    return;  // full: the GPU is far behind or lost the FIFO; drop rather than block
  Marker& m = q.ring[tail % Queue::kSize];
  m.block = block;
  m.offset = offset;
  std::memcpy(m.prefix.data(), prefix, offset < m.prefix.size() ? offset : m.prefix.size());
  m.prev_block = prev_block;
  std::memcpy(m.before.data(), before, m.before.size());
  m.value.store(value, std::memory_order_relaxed);
  q.tail.store(tail + 1, std::memory_order_release);
}

// FIFO reader: the 32 bytes read from guest address `block` were just copied to
// `dst`, which has room for kMaxExtraBytes more. Splices in this block's
// markers and returns how many bytes it added.
//
// Markers are queued in the order the game wrote the FIFO, which is the order
// the GPU reads it, so a marker for this block that is not at the head means
// the ones before it were never met, and they are dropped. That happens: while
// the game boots it fills its two FIFOs with frames it never hands to the GPU.
// Waiting for those instead held every later marker back, and none matched
// again. Only the newest kKeep are searched, so a long stretch of unread frames
// costs a bounded scan per block.
inline constexpr u32 kKeep = 48;
inline size_t Splice(u32 block, u8* dst, size_t block_size)
{
  Queue& q = g_queue;
  u8 orig[32];
  const size_t orig_size = block_size < sizeof(orig) ? block_size : sizeof(orig);
  std::memcpy(orig, dst, orig_size);
  const u32 last_block = g_last_block;
  const std::array<u8, 32> last_bytes = g_last_bytes;
  g_last_block = block;
  std::memcpy(g_last_bytes.data(), orig, orig_size);

  u32 head = q.head.load(std::memory_order_relaxed);
  const u32 tail = q.tail.load(std::memory_order_acquire);
  if (head == tail)
    return 0;
  if (tail - head > kKeep)
  {
    head = tail - kKeep;
    q.head.store(head, std::memory_order_release);
  }
  // This block's own markers: same address, the bytes the game had written in
  // front of each are the bytes this pass holds, and so is the block before --
  // when the reader came from there. After a jump (the game switched FIFOs)
  // there is no block before to compare.
  const auto mine = [&](const Marker& m) {
    return m.block == block && m.offset <= orig_size &&
           std::memcmp(orig, m.prefix.data(), m.offset) == 0 &&
           (last_block != m.prev_block || last_bytes == m.before);
  };
  u32 first = head;
  while (first != tail && !mine(q.ring[first % Queue::kSize]))
    ++first;
  if (first == tail)
    return 0;
  head = first;  // the ones passed over were never met
  size_t added = 0;
  size_t n = 0;
  while (head != tail && mine(q.ring[head % Queue::kSize]))
  {
    const Marker& m = q.ring[head % Queue::kSize];
    if (n < kMaxPerBlock)
    {
      u8* at = dst + m.offset + added;
      std::memmove(at + kCommandSize, at, block_size - m.offset);
      const u32 value = m.value.load(std::memory_order_acquire) & 0xffffffu;
      at[0] = kBpOpcode;
      at[1] = kLayerBpReg;
      at[2] = static_cast<u8>(value >> 16);
      at[3] = static_cast<u8>(value >> 8);
      at[4] = static_cast<u8>(value);
      added += kCommandSize;
      ++n;
    }
    ++head;
  }
  q.head.store(head, std::memory_order_release);
  return added;
}

inline void Reset()
{
  g_last_block = 0xFFFFFFFFu;
  g_queue.head.store(g_queue.tail.load(std::memory_order_acquire), std::memory_order_release);
}
}  // namespace RecompLayerMarkers
