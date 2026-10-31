package org.director64.tv;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.HashMap;
import java.util.Map;

/**
 * Controllers share the game's single input, mapped as the browser maps a standard gamepad
 * (platforms/web/site/gamepad.js): the most recently used device reports one controller sample (input.h buttons, stick about ±80). A DualShock 4 is Android's
 * standard gamepad; an N64-style pad maps the same way, its C buttons arriving as the right stick.
 * The TV remote's D-pad moves a cursor and its centre clicks.
 */
final class Pads {
  // runtime/interaction/input.h
  static final int A = 1, B = 2, LEFT = 4, RIGHT = 8, UP = 16, DOWN = 32, START = 64;
  static final int C_LEFT = 128, C_RIGHT = 256, C_UP = 512, C_DOWN = 1024;
  // A worn DualShock stick rests up to about a tenth off centre.
  static final float DEAD_ZONE = 0.12f;
  // The sample's stick reaches about ±80; input.c ignores ±8.
  static final int STICK_MIN = 9, STICK_MAX = 80;
  // L1 held: fine movement, for small hotspots.
  static final float FINE = 0.35f;
  // A remote that has been idle this long gives its port back.
  static final long REMOTE_IDLE_MS = 30_000;

  static final class Port {
    final int device;
    final String name;
    final boolean remote;
    int keys; // input.h bits from buttons and the D-pad keys
    boolean fine;
    float x, y, rx, ry, hatX, hatY;
    long lastInput;
    String last = "";

    Port(InputDevice device) {
      this.device = device.getId();
      this.name = device.getName();
      int sources = device.getSources();
      this.remote =
          (sources & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
              && (sources & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK;
    }

    int buttons() {
      int b = keys;
      if (hatX < -0.5f) b |= LEFT;
      if (hatX > 0.5f) b |= RIGHT;
      if (hatY < -0.5f) b |= UP;
      if (hatY > 0.5f) b |= DOWN;
      if (rx < -0.5f) b |= C_LEFT;
      if (rx > 0.5f) b |= C_RIGHT;
      if (ry < -0.5f) b |= C_UP;
      if (ry > 0.5f) b |= C_DOWN;
      return b;
    }
  }

  private Port active;
  private final Map<Integer, Port> byDevice = new HashMap<>();
  String lastInput = "";

  static boolean isController(InputDevice device) {
    if (device == null) return false;
    int sources = device.getSources();
    return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
        || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        || (sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD
        || (sources & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD;
  }

  /** A full keyboard, whose keys go to the game as keys rather than as a controller. */
  static boolean isKeyboard(InputDevice device) {
    return device != null
        && !device.isVirtual()
        && device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC
        && (device.getSources() & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD;
  }

  private Port port(InputDevice device) {
    Port port = byDevice.get(device.getId());
    if (port == null) {
      port = new Port(device);
      byDevice.put(device.getId(), port);
    }
    return port;
  }

  private void activate(Port port) {
    if (active == port) return;
    // Release the previous device before a new device's press. A later release
    // from the old device cannot clear the new device's held buttons.
    Native.pad(true, 0, 0, 0);
    active = port;
  }

  private static int bit(int keyCode) {
    switch (keyCode) {
      case KeyEvent.KEYCODE_BUTTON_A:
      case KeyEvent.KEYCODE_DPAD_CENTER:
      case KeyEvent.KEYCODE_ENTER:
        return A;
      case KeyEvent.KEYCODE_BUTTON_B:
        return B;
      case KeyEvent.KEYCODE_BUTTON_START:
        return START;
      case KeyEvent.KEYCODE_DPAD_UP:
        return UP;
      case KeyEvent.KEYCODE_DPAD_DOWN:
        return DOWN;
      case KeyEvent.KEYCODE_DPAD_LEFT:
        return LEFT;
      case KeyEvent.KEYCODE_DPAD_RIGHT:
        return RIGHT;
      default:
        return 0;
    }
  }

  /** A controller key; false when it is not one the game reads. */
  boolean key(KeyEvent event) {
    InputDevice device = event.getDevice();
    if (!isController(device)) return false;
    int bit = bit(event.getKeyCode());
    boolean fine = event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_L1;
    if (bit == 0 && !fine) return false;
    Port port = port(device);
    boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
    if (fine) port.fine = down;
    else if (down) port.keys |= bit;
    else port.keys &= ~bit;
    port.lastInput = event.getEventTime();
    port.last = KeyEvent.keyCodeToString(event.getKeyCode()) + (down ? " down" : " up");
    lastInput = port.name + ": " + port.last;
    if (down && event.getRepeatCount() == 0) activate(port);
    if (active == port) send(port);
    return true;
  }

  /** A controller's sticks and hat; false when the event is not a controller's. */
  boolean motion(MotionEvent event) {
    InputDevice device = event.getDevice();
    if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
        || !isController(device)
        || event.getAction() != MotionEvent.ACTION_MOVE) return false;
    Port port = port(device);
    float oldX=port.x,oldY=port.y,oldRx=port.rx,oldRy=port.ry;
    int oldButtons=port.buttons();
    port.x = event.getAxisValue(MotionEvent.AXIS_X);
    port.y = event.getAxisValue(MotionEvent.AXIS_Y);
    port.rx = event.getAxisValue(MotionEvent.AXIS_Z);
    port.ry = event.getAxisValue(MotionEvent.AXIS_RZ);
    port.hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
    port.hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
    port.lastInput = event.getEventTime();
    port.last =
        String.format(
            java.util.Locale.ROOT,
            "x %.2f y %.2f z %.2f rz %.2f hat %.0f/%.0f",
            port.x, port.y, port.rx, port.ry, port.hatX, port.hatY);
    lastInput = port.name + ": " + port.last;
    boolean moved=(oldX!=port.x||oldY!=port.y) && Math.hypot(port.x,port.y)>DEAD_ZONE;
    boolean rightMoved=(oldRx!=port.rx||oldRy!=port.ry)
        && (Math.abs(port.rx)>.5f||Math.abs(port.ry)>.5f);
    if (moved||rightMoved||(port.buttons()&~oldButtons)!=0) activate(port);
    if (active == port) send(port);
    return true;
  }

  // The stick in the sample's units: x right, y up. Radial dead zone, then a
  // gentle curve so small deflections stay precise.
  static int[] consoleStick(float x, float y, float scale) {
    double r = Math.hypot(x, y);
    if (!(r > DEAD_ZONE)) return new int[] {0, 0};
    double m = Math.pow(Math.min(1, (r - DEAD_ZONE) / (1 - DEAD_ZONE)), 1.5);
    double magnitude = STICK_MIN + (STICK_MAX - STICK_MIN) * m * scale;
    return new int[] {(int) Math.round(x / r * magnitude), (int) Math.round(-y / r * magnitude)};
  }

  private void send(Port port) {
    int[] stick = consoleStick(port.x, port.y, port.fine ? FINE : 1);
    Native.pad(true, port.buttons(), stick[0], stick[1]);
  }

  /** A device left; held buttons from it are released. */
  void removed(int deviceId) {
    Port port = byDevice.remove(deviceId);
    if (port != null && port == active) {
      active = null;
      Native.pad(false, 0, 0, 0);
    }
  }

  /** Idle remotes do not retain ownership forever. */
  void expire(long now) {
    for (Port port : new java.util.ArrayList<>(byDevice.values()))
      if (port.remote && port.keys == 0 && now - port.lastInput > REMOTE_IDLE_MS)
        removed(port.device);
  }

  /** Whatever was held belongs to the modal that opens. */
  void release() {
    for (Port port : byDevice.values()) {
      port.keys = 0;
      port.fine = false;
      port.x = port.y = port.rx = port.ry = port.hatX = port.hatY = 0;
    }
    Native.pad(active != null, 0, 0, 0);
  }

  String describe() {
    return active == null ? "" : active.name + "\n";
  }
}
