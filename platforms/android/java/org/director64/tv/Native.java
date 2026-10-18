package org.director64.tv;

import android.content.res.AssetManager;
import android.view.Surface;

/** The game's native runtime (platforms/android/jni/main.c). */
final class Native {
  // What holds the game (main.c HOLD_*).
  static final int HOLD_PAUSED = 1, HOLD_MENU = 2, HOLD_TEXT = 4;

  private Native() {}

  /** Starts the game thread on the APK's assets; the save is the console's flash image. */
  static native void start(GameActivity activity, AssetManager assets, String savePath, float volume);

  static native void stop();

  static native void surface(Surface surface);

  static native void hold(int reason, boolean on);

  /** One controller port as an N64 controller sample (runtime/interaction/input.h). */
  static native void pad(int port, boolean connected, int buttons, int stickX, int stickY);

  /** The mouse in stage pixels; buttons as input.h bits (left A, right B). */
  static native void mouse(int x, int y, int buttons, boolean inside);

  /** A key as a desktop projector receives it: Macintosh key code and character. */
  static native void key(int code, int character, boolean down);

  static native boolean commitText(int request, byte[] windows1252);

  static native void cancelText(int request);

  /** A character typed into a key field (Mucklas' name) as the game's key presses; 8 deletes. */
  static native void typeKey(int request, int character);

  static native void volume(float level);

  static native String diagnostics();
}
