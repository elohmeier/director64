#if DG_D10 && !defined(DIRECTOR64_MUCKLAS)
// The Macintosh virtual key code a typed character arrives with.
static unsigned typed_key_code(unsigned c) {
  static const uint8_t letters[] = {0,  11, 8,  2,  14, 3,  5,  4,  34,
                                    38, 40, 37, 46, 45, 31, 35, 12, 15,
                                    1,  17, 32, 9,  13, 7,  16, 6};
  if (c >= 'a' && c <= 'z')
    c -= 'a' - 'A';
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  return c == 8 ? 51 : c == 13 ? 36 : c == ' ' ? 49 : c == '-' ? 27 : 18;
}
#endif
// Single-owner hosts reserve space before submitting a press/release pair.
EMSCRIPTEN_KEEPALIVE unsigned d64_key_capacity(void) {
#if DG_CAP_KEYBOARD
  return booted && !values.failed && director.key_count <= 32
             ? 32 - director.key_count
             : 0;
#else
  return 32;
#endif
}
// One character typed into a key field, pressed and released as the host's
// keyboard sends it; 8 deletes.
EMSCRIPTEN_KEEPALIVE void d64_type_key(unsigned character, int down) {
#ifdef DIRECTOR64_MUCKLAS
  if (booted && !values.failed && character <= 255 && director.key_count < 32)
    dg_key(&director, mucklas_key_code(character), character, down != 0);
#elif DG_D10
  if (booted && !values.failed && character <= 255 && director.key_count < 32)
    dg_key(&director, typed_key_code(character), character, down != 0);
#else
  (void)character;
  (void)down;
#endif
}
// A key of the page's keyboard, as a desktop projector receives it: the
// Macintosh virtual key code and the character it types (dg_key).
EMSCRIPTEN_KEEPALIVE void d64_key(unsigned code, unsigned character, int down) {
#if DG_CAP_KEYBOARD
  // The page may outrun the score (a held key repeats, typing is fast);
  // what the queue cannot take is dropped rather than failing the game.
  if (booted && !values.failed && code < 128 && character <= 255 &&
      director.key_count < 32)
    dg_key(&director, code, character, down != 0);
#else
  (void)code;
  (void)character;
  (void)down;
#endif
}
