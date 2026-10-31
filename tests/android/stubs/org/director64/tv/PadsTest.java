package org.director64.tv;
import android.view.*;
public final class PadsTest {
  static void check(boolean value){if(!value)throw new AssertionError();}
  static int[] last(){return Native.pads.get(Native.pads.size()-1);}
  static KeyEvent key(int device,int code,boolean down){return new KeyEvent(down?0:1,device,0,0,code);}
  public static void main(String[] args) {
    InputDevice one=new InputDevice(1,InputDevice.SOURCE_GAMEPAD|InputDevice.SOURCE_JOYSTICK);
    new InputDevice(2,InputDevice.SOURCE_GAMEPAD|InputDevice.SOURCE_JOYSTICK);
    new InputDevice(3,InputDevice.SOURCE_DPAD);
    Pads pads=new Pads();
    check(pads.key(key(1,KeyEvent.KEYCODE_BUTTON_A,true)));check(last()[1]==Pads.A);
    pads.key(key(2,KeyEvent.KEYCODE_BUTTON_B,true));check(last()[1]==Pads.B);
    int count=Native.pads.size();
    pads.key(key(1,KeyEvent.KEYCODE_BUTTON_A,false));check(Native.pads.size()==count&&last()[1]==Pads.B);
    pads.removed(1);check(Native.pads.size()==count);
    pads.key(key(2,KeyEvent.KEYCODE_BUTTON_B,false));check(last()[1]==0);
    pads.key(key(3,KeyEvent.KEYCODE_DPAD_UP,true));check(last()[1]==Pads.UP);
    pads.motion(new MotionEvent(one,0,0));check(last()[1]==Pads.UP);
    pads.motion(new MotionEvent(one,1,0));check(last()[1]==0&&last()[2]==80);
    check(pads.describe().equals("device1\n"));
    pads.removed(1);check(last()[0]==0&&last()[1]==0);
    pads.key(key(3,KeyEvent.KEYCODE_DPAD_UP,false));
    pads.key(key(3,KeyEvent.KEYCODE_DPAD_CENTER,true));check(last()[1]==Pads.A);
    pads.release();check(last()[1]==0&&last()[2]==0&&last()[3]==0);
    pads.expire(40000);check(last()[0]==0);
    System.out.println("Android controller: one input, handoff releases, idle devices, disconnect and modal clearing pass");
  }
}
