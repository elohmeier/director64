package android.view;
import java.util.HashMap;
public final class InputDevice {
  public static final int SOURCE_GAMEPAD=0x401,SOURCE_JOYSTICK=0x1000010,
      SOURCE_DPAD=0x201,SOURCE_KEYBOARD=0x101,KEYBOARD_TYPE_ALPHABETIC=2;
  private static final HashMap<Integer,InputDevice> DEVICES=new HashMap<>();
  private final int id,sources;
  public InputDevice(int id,int sources){this.id=id;this.sources=sources;DEVICES.put(id,this);}
  public static InputDevice getDevice(int id){return DEVICES.get(id);}
  public int getId(){return id;}
  public int getSources(){return sources;}
  public String getName(){return "device"+id;}
  public int getKeyboardType(){return 0;}
  public boolean isVirtual(){return false;}
}
