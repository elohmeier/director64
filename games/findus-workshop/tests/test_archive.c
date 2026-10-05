#include "archive.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char flash[2 * ARCHIVE_BYTES], backup[2 * ARCHIVE_BYTES];
static archive_t a, b;
static size_t cut = ARCHIVE_BYTES;
static unsigned writes;
static bool read_(void *ctx, size_t offset, void *data, size_t length) {
  (void)ctx;
  assert(offset + length <= sizeof(flash));
  memcpy(data, flash + offset, length);
  return true;
}
static bool write_(void *ctx, size_t offset, const void *data, size_t length) {
  (void)ctx;
  assert(offset + length <= sizeof(flash));
  writes++;
  memset(flash + offset, 255, ARCHIVE_BYTES);
  size_t written = cut < length ? cut : length;
  memcpy(flash + offset, data, written);
  return written == length;
}
static void check(archive_t *store, const char *name, const char *expected) {
  char data[16384];
  unsigned length;
  assert(archive_read(store, name, data, sizeof(data), &length));
  assert(length == strlen(expected) && !strcmp(data, expected));
}
static void checksum(unsigned char *block) {
  memset(block + 16, 0, 4);
  uint32_t crc = UINT32_MAX;
  for (unsigned i = 0; i < ARCHIVE_BYTES; i++) {
    crc ^= block[i];
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  crc ^= UINT32_MAX;
  for (unsigned i = 0; i < 4; i++) block[16 + i] = crc >> (24 - i * 8);
}
static void upgrade(save_backend_t backend) {
  memcpy(flash, backup, sizeof(flash));
  // A real V1 archive uses zero reserved bytes and the same nine payloads.
  for (unsigned slot = 0; slot < 2; slot++) {
    unsigned char *block = flash + slot * ARCHIVE_BYTES;
    block[7] = 1;
    memset(block + 68, 0, 4);
    checksum(block);
  }
  assert(archive_load(&a, backend) == SAVE_VALID && a.generation == 3);
  // Writing through a V1 store re-journals it as V2. Every payload but the one
  // that was written keeps its contents and its offset.
  assert(archive_write(&a, "musikfil.txt", "second\r", 7));
  assert(archive_load(&b, backend) == SAVE_VALID);
  assert(b.generation == 4 && flash[b.active_slot * ARCHIVE_BYTES + 7] == 2);
  check(&b, "vakt2.txt", "19\r4\r[1,2]\r[0]\r");
  check(&b, "byggfil.txt", "construction\r");
  check(&b, "musikfil.txt", "second\r");
  memcpy(backup, flash, sizeof(flash));
  for (unsigned i = 0; i < 640; i++) {
    cut = i < 128 ? i : (i - 128) * 128;
    memcpy(flash, backup, sizeof(flash));
    assert(archive_load(&a, backend) == SAVE_VALID);
    assert(!archive_write(&a, "musikfil.txt", "third\r", 6));
    check(&a, "musikfil.txt", "second\r"); // A failed write stays readable.
    assert(archive_load(&b, backend) == SAVE_VALID && b.generation == 4);
    assert(!memcmp(a.data, b.data, sizeof(a.data)));
  }
  cut = ARCHIVE_BYTES;
  assert(archive_write(&a, "musikfil.txt", "third\r", 6)); // Retry, same store.
  assert(archive_load(&b, backend) == SAVE_VALID && b.generation == 5);
  // Unknown reserved header values never win the journal.
  unsigned char *block = flash + b.active_slot * ARCHIVE_BYTES;
  block[71] = 2; checksum(block);
  assert(archive_load(&a, backend) == SAVE_VALID && a.generation == 4);
  block[71] = 0; block[72] = 1; checksum(block);
  assert(archive_load(&a, backend) == SAVE_VALID && a.generation == 4);
}
int main(void) {
  save_backend_t backend = {read_, write_, NULL};
  memset(flash, 255, sizeof(flash));
  assert(archive_load(&a, backend) == SAVE_BLANK);
  assert(archive_file_id("VAKT7.TXT") == 6 &&
         archive_file_id("../vakt1.txt") == -1);
  assert(archive_file_id("vakt1.txtx") == -1);
  assert(archive_write(&a, "vakt2.txt", "19\r4\r[1,2]\r[0]\r", 15));
  assert(archive_write(&a, "ByggFil.txt", "construction\r", 13));
  assert(archive_write(&a, "MusikFil.txt", "music\r", 6));
  unsigned before = writes;
  // Rewriting a file with its current contents never costs a FlashRAM write.
  assert(archive_write(&a, "musikfil.txt", "music\r", 6) && writes == before);
  assert(archive_load(&b, backend) == SAVE_VALID && b.generation == 3);
  check(&b, "byggfil.txt", "construction\r");
  check(&b, "musikfil.txt", "music\r");
  memcpy(backup, flash, sizeof(flash));
  // Every 128-byte FlashRAM page boundary, plus each byte of the header.
  for (unsigned i = 0; i < 640; i++) {
    cut = i < 128 ? i : (i - 128) * 128;
    memcpy(flash, backup, sizeof(flash));
    assert(archive_load(&a, backend) == SAVE_VALID);
    assert(!archive_write(&a, "musikfil.txt", "replacement\r", 12));
    assert(archive_load(&b, backend) == SAVE_VALID && b.generation == 3);
    check(&b, "musikfil.txt", "music\r");
  }
  cut = ARCHIVE_BYTES;
  memcpy(flash, backup, sizeof(flash));
  flash[ARCHIVE_BYTES + 150] ^= 1; // Damaged previous generation must not win.
  assert(archive_load(&a, backend) == SAVE_VALID && a.generation == 3);
  upgrade(backend);
  memset(flash, 255, sizeof(flash));
  save_store_t old;
  assert(save_load(&old, backend) == SAVE_BLANK);
  old.model.profiles[4].feathers = 17;
  old.model.profiles[4].coins = 8;
  old.model.profiles[4].treasure[34] = 33;
  old.model.profiles[4].map[8] = 1;
  assert(save_commit(&old, &old.model));
  assert(archive_load(&a, backend) == SAVE_VALID && a.migrated);
  char text[512];
  unsigned length;
  assert(archive_read(&a, "vakt5.txt", text, sizeof(text), &length));
  assert(!strncmp(text, "17\r8\r", 5) && strstr(text, ",33]\r") &&
         strstr(text, ",1]\r"));
  assert(archive_write(&a, "Musikfil.txt", "new\r", 4));
  assert(archive_load(&b, backend) == SAVE_VALID && !b.migrated &&
         b.generation == 2);
  check(&b, "vakt5.txt", text);
  memset(flash, 0x34, sizeof(flash));
  assert(archive_load(&a, backend) == SAVE_CORRUPT);
  assert(!archive_write(&a, "vakt1.txt", "x", 1));
  assert(flash[0] == 0x34);
  puts("archive: nine files, V1/legacy migration, V1 upgrade, 1280 torn writes, corrupt-save "
       "protection OK");
}
