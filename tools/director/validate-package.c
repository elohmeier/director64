// Exercise the production loader before a converted package enters an APK.
#include "package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int main(int argc, char **argv) {
  if (argc != 3 || strlen(argv[2]) != DG_PACKAGE_ABI_BYTES * 2) {
    fputs("usage: validate-package GAME.d64p ABI_HEX\n", stderr);
    return 2;
  }
  uint8_t abi[DG_PACKAGE_ABI_BYTES];
  for (unsigned i = 0; i < sizeof(abi); i++) {
    int hi = hex_digit(argv[2][i * 2]), lo = hex_digit(argv[2][i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      fputs("invalid package ABI hex\n", stderr);
      return 2;
    }
    abi[i] = (uint8_t)(hi * 16 + lo);
  }
  FILE *input = fopen(argv[1], "rb");
  if (!input) {
    perror(argv[1]);
    return 2;
  }
  long length;
  if (fseek(input, 0, SEEK_END) || (length = ftell(input)) < 0 ||
      fseek(input, 0, SEEK_SET)) {
    fputs("cannot read package size\n", stderr);
    fclose(input);
    return 2;
  }
  uint8_t *bytes = malloc(length ? (size_t)length : 1);
  if (!bytes || fread(bytes, 1, (size_t)length, input) != (size_t)length) {
    fputs("cannot read complete package\n", stderr);
    free(bytes);
    fclose(input);
    return 2;
  }
  fclose(input);
  dg_package_t package;
  char error[256];
  bool valid = dg_package_load(&package, bytes, (size_t)length, abi, error, sizeof(error));
  free(bytes);
  if (!valid) {
    fprintf(stderr, "package validation failed: %s\n", error);
    return 1;
  }
  printf("{\"movies\":%u,\"globals\":%u,\"bytes\":%ld}\n",
         package.movie_count, package.global_count, length);
  dg_package_free(&package);
  return 0;
}
