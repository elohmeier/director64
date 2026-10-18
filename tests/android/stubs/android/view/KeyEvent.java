package android.view; public class KeyEvent { public static final int ACTION_DOWN=0,ACTION_UP=1;public static final int KEYCODE_0=2000;
public static final int KEYCODE_A=1000;
public static final int KEYCODE_APOSTROPHE=3002;
public static final int KEYCODE_BACKSLASH=3003;
public static final int KEYCODE_COMMA=3004;
public static final int KEYCODE_DEL=3005;
public static final int KEYCODE_DPAD_DOWN=3006;
public static final int KEYCODE_DPAD_LEFT=3007;
public static final int KEYCODE_DPAD_RIGHT=3008;
public static final int KEYCODE_DPAD_UP=3009;
public static final int KEYCODE_ENTER=3010;
public static final int KEYCODE_EQUALS=3011;
public static final int KEYCODE_ESCAPE=3012;
public static final int KEYCODE_FORWARD_DEL=3013;
public static final int KEYCODE_GRAVE=3014;
public static final int KEYCODE_LEFT_BRACKET=3015;
public static final int KEYCODE_MINUS=3016;
public static final int KEYCODE_MOVE_END=3017;
public static final int KEYCODE_MOVE_HOME=3018;
public static final int KEYCODE_NUMPAD_ENTER=3019;
public static final int KEYCODE_PAGE_DOWN=3020;
public static final int KEYCODE_PAGE_UP=3021;
public static final int KEYCODE_PERIOD=3022;
public static final int KEYCODE_RIGHT_BRACKET=3023;
public static final int KEYCODE_SEMICOLON=3024;
public static final int KEYCODE_SLASH=3025;
public static final int KEYCODE_SPACE=3026;
public static final int KEYCODE_TAB=3027;private final int action, device, modifiers, repeat;
  public KeyEvent(int action, int device, int modifiers, int repeat) {
    this.action=action;this.device=device;this.modifiers=modifiers;this.repeat=repeat;
  }
  public int getKeyCode(){return KEYCODE_A;}
  public int getAction(){return action;}
  public int getDeviceId(){return device;}
  public boolean isAltPressed(){return (modifiers&1)!=0;}
  public boolean isCtrlPressed(){return (modifiers&2)!=0;}
  public boolean isMetaPressed(){return (modifiers&4)!=0;}
  public int getRepeatCount(){return repeat;}
  public int getUnicodeChar(){return 97;}
}
