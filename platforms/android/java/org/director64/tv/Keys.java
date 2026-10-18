package org.director64.tv;

import android.util.SparseIntArray;
import android.view.KeyEvent;
import java.util.HashMap;
import java.util.Map;

/**
 * A keyboard's keys as a desktop projector receives them (platforms/web/site/player.js MAC_KEYS):
 * the Macintosh virtual key code of the key's position and the character the layout types.
 */
final class Keys {
  private static final SparseIntArray MAC = new SparseIntArray();
  private static final SparseIntArray SPECIAL = new SparseIntArray();
  private static final Map<Integer, SparseIntArray> HELD = new HashMap<>();

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

  private static boolean ownedElsewhere(int device, int code) {
    for (Map.Entry<Integer, SparseIntArray> owner : HELD.entrySet())
      if (owner.getKey() != device && owner.getValue().indexOfKey(code) >= 0) return true;
    return false;
  }

  /** Disconnects and holds must release keys even when no key-up arrives. */
  static void releaseDevice(int device) {
    SparseIntArray keys = HELD.remove(device);
    if (keys == null) return;
    for (int i = 0; i < keys.size(); i++) {
      int code = keys.keyAt(i);
      if (!ownedElsewhere(device, code)) Native.key(code, keys.valueAt(i), false);
    }
  }

  static void releaseAll() {
    for (Integer device : HELD.keySet().toArray(new Integer[0])) releaseDevice(device);
  }

  /** Forwards a keyboard key to the game; false when it has no Macintosh code. */
  static boolean forward(KeyEvent event) {
    int key = event.getKeyCode();
    int code = MAC.get(key, -1), device = event.getDeviceId();
    if (code < 0) return false;
    SparseIntArray keys = HELD.get(device);
    // Modifier state at release need not match the original press.
    if (event.getAction() == KeyEvent.ACTION_UP) {
      if (keys == null || keys.indexOfKey(code) < 0) return false;
      int character = keys.get(code);
      keys.delete(code);
      if (keys.size() == 0) HELD.remove(device);
      if (!ownedElsewhere(device, code)) Native.key(code, character, false);
      return true;
    }
    if (event.isCtrlPressed() || event.isMetaPressed() || event.isAltPressed()) return false;
    if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
    if (event.getRepeatCount() > 0 || (keys != null && keys.indexOfKey(code) >= 0)) return true;
    int character = SPECIAL.get(key, -1);
    if (character < 0) {
      int unicode = event.getUnicodeChar();
      character = unicode > 0 && unicode <= 255 ? unicode : 0;
    }
    if (keys == null) { keys = new SparseIntArray(); HELD.put(device, keys); }
    keys.put(code, character);
    if (!ownedElsewhere(device, code)) Native.key(code, character, true);
    return true;
  }
}
