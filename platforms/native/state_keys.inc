/* Property keys in the probe's state line, folded to lower case. A property
 * name is case-insensitive in Lingo, and the compiler keeps the spelling of
 * the script that declared it, so a state that named keys as spelled would
 * shift under every journey and fuzz baseline whenever a script's spelling
 * differs from its use. Included by each game's tests/director_probe.c after
 * its quoted() helper.
 */
static void quoted_key(const char *text) {
  char folded[256];
  unsigned at = 0;
  for (; text[at] && at + 1 < sizeof(folded); at++)
    folded[at] = text[at] >= 'A' && text[at] <= 'Z' ? (char)(text[at] + 32) : text[at];
  folded[at] = 0;
  quoted(folded);
}
