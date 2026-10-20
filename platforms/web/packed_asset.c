#include <stdint.h>
#include <string.h>
#include <zlib.h>

// D64Z: big-endian decoded length and raw deflate. Assets stay compressed
// in web builds, just as in APKs; only a requested record is inflated.
uint32_t d64_unpack_asset(const uint8_t *data, uint32_t size, uint8_t *out, uint32_t cap) {
  if (size < 8 || memcmp(data, "D64Z", 4)) return 0;
  uint32_t length = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 |
                    (uint32_t)data[6] << 8 | data[7];
  if (!length || length > cap) return 0;
  z_stream z = {0};
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return 0;
  z.next_in = (Bytef *)(data + 8); z.avail_in = size - 8;
  z.next_out = out; z.avail_out = cap;
  int status = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  return status == Z_STREAM_END && z.total_out == length && z.total_in == size - 8 ? length : 0;
}
