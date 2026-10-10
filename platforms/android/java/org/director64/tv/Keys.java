package org.director64.tv;

import android.util.SparseIntArray;
import android.view.KeyEvent;

/**
 * A keyboard's keys as a desktop projector receives them (platforms/web/site/player.js MAC_KEYS):
 * the Macintosh virtual key code of the key's position and the character the layout types.
 */
final class Keys {
  private static final SparseIntArray MAC = new SparseIntArray();
  private static final SparseIntArray SPECIAL = new SparseIntArray();
  private static final SparseIntArray HELD = new SparseIntArray();

  private static void map(int key, int mac) {
    MAC.put(key, mac);
  }

  static {
    int[] letters = {0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};
    for (int i = 0; i < 26; i++) map(KeyEvent.KEYCODE_A + i, letters[i]);
    int[] digits = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};
    for (int i = 0; i < 10; i++) map(KeyEvent.KEYCODE_0 + i, digits[i]);
    map(KeyEvent.KEYCODE_EQUALS, 24);
    map(KeyEvent.KEYCODE_MINUS, 27);
    map(KeyEvent.KEYCODE_RIGHT_BRACKET, 30);
    map(KeyEvent.KEYCODE_LEFT_BRACKET, 33);
    map(KeyEvent.KEYCODE_ENTER, 36);
    map(KeyEvent.KEYCODE_APOSTROPHE, 39);
    map(KeyEvent.KEYCODE_SEMICOLON, 41);
    map(KeyEvent.KEYCODE_BACKSLASH, 42);
    map(KeyEvent.KEYCODE_COMMA, 43);
    map(KeyEvent.KEYCODE_SLASH, 44);
    map(KeyEvent.KEYCODE_PERIOD, 47);
    map(KeyEvent.KEYCODE_TAB, 48);
    map(KeyEvent.KEYCODE_SPACE, 49);
    map(KeyEvent.KEYCODE_GRAVE, 50);
    map(KeyEvent.KEYCODE_DEL, 51);
    map(KeyEvent.KEYCODE_ESCAPE, 53);
    map(KeyEvent.KEYCODE_NUMPAD_ENTER, 76);
    map(KeyEvent.KEYCODE_MOVE_HOME, 115);
    map(KeyEvent.KEYCODE_PAGE_UP, 116);
    map(KeyEvent.KEYCODE_FORWARD_DEL, 117);
    map(KeyEvent.KEYCODE_MOVE_END, 119);
    map(KeyEvent.KEYCODE_PAGE_DOWN, 121);
    map(KeyEvent.KEYCODE_DPAD_LEFT, 123);
    map(KeyEvent.KEYCODE_DPAD_RIGHT, 124);
    map(KeyEvent.KEYCODE_DPAD_DOWN, 125);
    map(KeyEvent.KEYCODE_DPAD_UP, 126);
    SPECIAL.put(KeyEvent.KEYCODE_ENTER, 13);
    SPECIAL.put(KeyEvent.KEYCODE_NUMPAD_ENTER, 13);
    SPECIAL.put(KeyEvent.KEYCODE_DEL, 8);
    SPECIAL.put(KeyEvent.KEYCODE_TAB, 9);
    SPECIAL.put(KeyEvent.KEYCODE_ESCAPE, 27);
    SPECIAL.put(KeyEvent.KEYCODE_DPAD_LEFT, 28);
    SPECIAL.put(KeyEvent.KEYCODE_DPAD_RIGHT, 29);
    SPECIAL.put(KeyEvent.KEYCODE_DPAD_UP, 30);
    SPECIAL.put(KeyEvent.KEYCODE_DPAD_DOWN, 31);
    SPECIAL.put(KeyEvent.KEYCODE_FORWARD_DEL, 127);
  }

  private Keys() {}

  /** Forwards a keyboard key to the game; false when it has no Macintosh code. */
  static boolean forward(KeyEvent event) {
    int key = event.getKeyCode();
    int code = MAC.get(key, -1);
    if (code < 0 || event.isCtrlPressed() || event.isMetaPressed() || event.isAltPressed()) return false;
    if (event.getAction() == KeyEvent.ACTION_DOWN) {
      if (event.getRepeatCount() > 0 || HELD.indexOfKey(code) >= 0) return true;
      int character = SPECIAL.get(key, -1);
      if (character < 0) {
        int unicode = event.getUnicodeChar();
        character = unicode > 0 && unicode <= 255 ? unicode : 0;
      }
      HELD.put(code, character);
      Native.key(code, character, true);
    } else if (event.getAction() == KeyEvent.ACTION_UP && HELD.indexOfKey(code) >= 0) {
      Native.key(code, HELD.get(code), false);
      HELD.delete(code);
    }
    return true;
  }
}
