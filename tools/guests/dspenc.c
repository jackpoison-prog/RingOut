/* GameCube DSP-ADPCM encoder for the Spawn converter (tools/spawn/sndconv.py).
 *
 * Exhaustive search, exactly as the numpy encoder it replaces: every 14-sample frame is tried with each
 * of the 8 coefficient pairs x 16 scales (128 lanes, lane = pair*16 + scale), each lane running its own
 * decoder history from the previous frame's choice; the lane with the smallest sum of squared errors
 * wins (the first one on a tie). Output must stay byte-identical to the numpy version: rounding is
 * rint() (half to even, like numpy.round), all arithmetic in 64 bits.
 *
 * Protocol on stdin/stdout, little-endian: request = u32 n, 16 x s16 coefficients, n x s16 samples;
 * reply = u32 frames, frames x 8 bytes. A request with n = 0 ends the program.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int get(void* p, size_t n) { return fread(p, 1, n, stdin) == n; }
static uint32_t le32(const unsigned char* b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static int64_t asr11(int64_t v) { return v >= 0 ? v >> 11 : -((-v - 1) >> 11) - 1; } /* floor(v / 2048) */
static int64_t clamp(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

int main(void)
{
#ifdef _WIN32
  /* Windows opens both pipes in text mode: a 0x1A sample byte ends the input and every 0x0A written
   * gains a 0x0D. The protocol is binary. */
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  for (;;)
  {
    unsigned char hdr[4 + 32];
    if (!get(hdr, 4)) return 1;
    uint32_t n = le32(hdr);
    if (n == 0) return 0;
    if (!get(hdr + 4, 32)) return 1;
    int64_t c1[128], c2[128], sc[128];
    for (int k = 0; k < 8; k++)
    {
      int16_t a, b;
      memcpy(&a, hdr + 4 + 4 * k, 2);
      memcpy(&b, hdr + 6 + 4 * k, 2);
      for (int s = 0; s < 16; s++) { c1[k * 16 + s] = a; c2[k * 16 + s] = b; sc[k * 16 + s] = s; }
    }
    uint32_t frames = (n + 13) / 14;
    int16_t* x = calloc((size_t)frames * 14, 2);
    unsigned char* out = malloc((size_t)frames * 8 + 4);
    if (!x || !out || !get(x, (size_t)n * 2)) return 1;
    out[0] = frames; out[1] = frames >> 8; out[2] = frames >> 16; out[3] = frames >> 24;
    int64_t h1 = 0, h2 = 0;
    for (uint32_t f = 0; f < frames; f++)
    {
      const int16_t* blk = x + (size_t)f * 14;
      int64_t H1[128], H2[128], err[128];
      int8_t nibs[128][14];
      for (int l = 0; l < 128; l++)
      {
        H1[l] = h1; H2[l] = h2; err[l] = 0;
        for (int i = 0; i < 14; i++)
        {
          int64_t pred = asr11(c1[l] * H1[l] + c2[l] * H2[l] + 1024);
          int64_t d = blk[i] - pred;
          int64_t nib = clamp((int64_t)rint((double)d / (double)(1 << sc[l])), -8, 7);
          int64_t v = clamp(asr11(nib * (1 << sc[l]) * 2048 + 1024 + c1[l] * H1[l] + c2[l] * H2[l]), -32768, 32767);
          int64_t e = blk[i] - v;
          err[l] += e * e;
          nibs[l][i] = (int8_t)nib;
          H2[l] = H1[l]; H1[l] = v;
        }
      }
      int best = 0;
      for (int l = 1; l < 128; l++) if (err[l] < err[best]) best = l;
      unsigned char* o = out + 4 + (size_t)f * 8;
      o[0] = (unsigned char)(((best / 16) << 4) | (best % 16));
      for (int i = 0; i < 14; i += 2) o[1 + i / 2] = (unsigned char)((nibs[best][i] & 15) << 4 | (nibs[best][i + 1] & 15));
      h1 = H1[best]; h2 = H2[best];
    }
    if (fwrite(out, 1, (size_t)frames * 8 + 4, stdout) != (size_t)frames * 8 + 4) return 1;
    fflush(stdout);
    free(x); free(out);
  }
}
