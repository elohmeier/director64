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
public static final int KEYCODE_TAB=3027;
public static final int KEYCODE_META_LEFT=3028,KEYCODE_META_RIGHT=3029;
public static final int KEYCODE_SHIFT_LEFT=3030,KEYCODE_SHIFT_RIGHT=3031;
public static final int KEYCODE_ALT_LEFT=3032,KEYCODE_ALT_RIGHT=3033;
public static final int KEYCODE_CTRL_LEFT=3034,KEYCODE_CTRL_RIGHT=3035;
public static final int META_CTRL_MASK=2,META_META_MASK=4;
public static final int KEYCODE_BUTTON_A=3040,KEYCODE_BUTTON_B=3041,
    KEYCODE_BUTTON_START=3042,KEYCODE_DPAD_CENTER=3043,KEYCODE_BUTTON_L1=3044;
public InputDevice getDevice(){return InputDevice.getDevice(device);}
public long getEventTime(){return 1000;}
public static String keyCodeToString(int code){return Integer.toString(code);}
private final int action, device, modifiers, repeat,key;
  public KeyEvent(int action, int device, int modifiers, int repeat) {
    this(action,device,modifiers,repeat,KEYCODE_A);
  }
  public KeyEvent(int action,int device,int modifiers,int repeat,int key) {
    this.action=action;this.device=device;this.modifiers=modifiers;this.repeat=repeat;this.key=key;
  }
  public int getKeyCode(){return key;}
  public int getAction(){return action;}
  public int getDeviceId(){return device;}
  public boolean isAltPressed(){return (modifiers&1)!=0;}
  public boolean isCtrlPressed(){return (modifiers&2)!=0;}
  public boolean isMetaPressed(){return (modifiers&4)!=0;}
  public int getRepeatCount(){return repeat;}
  public int getMetaState(){return modifiers;}
  public int getUnicodeChar(){return getUnicodeChar(modifiers);}
  public int getUnicodeChar(int meta){return key>=KEYCODE_META_LEFT?0:(meta&2)!=0?1:(meta&8)!=0?65:97;}
}
