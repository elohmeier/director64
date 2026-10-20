#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
uint32_t d64_unpack_asset(const uint8_t *, uint32_t, uint8_t *, uint32_t);
int main(void) {
  uint8_t raw[4096], packed[8192] = {'D','6','4','Z',0,0,16,0}, out[4096];
  for (unsigned i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)(i % 29);
  z_stream z = {0}; assert(deflateInit2(&z, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
  z.next_in = raw; z.avail_in = sizeof(raw); z.next_out = packed + 8; z.avail_out = sizeof(packed) - 8;
  assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
  unsigned size = (unsigned)z.total_out + 8; deflateEnd(&z);
  assert(d64_unpack_asset(packed, size, out, sizeof(out)) == sizeof(raw));
  assert(!memcmp(raw, out, sizeof(raw)));
  assert(!d64_unpack_asset(packed, size, out, sizeof(out) - 1));
  assert(!d64_unpack_asset(packed, size - 1, out, sizeof(out)));
  assert(!d64_unpack_asset(packed, size + 1, out, sizeof(out)));
  packed[7] = 1; assert(!d64_unpack_asset(packed, size, out, sizeof(out)));
  assert(!d64_unpack_asset(packed, 4, out, sizeof(out)));
  puts("Packed web assets: exact inflate, capacity, truncation and trailing data passed");
}
