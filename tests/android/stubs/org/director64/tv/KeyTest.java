package org.director64.tv;
import android.view.KeyEvent;
public class KeyTest {
  static void check(boolean condition){if(!condition)throw new AssertionError();}
  public static void main(String[] args) {
    // Release must match the original press despite changing modifiers.
    for(int modifier:new int[]{1,2,4}) {
      int downs=Native.down, ups=Native.up;
      check(Keys.forward(new KeyEvent(0,11,0,0)));
      check(Keys.forward(new KeyEvent(1,11,modifier,0)));
      check(Keys.forward(new KeyEvent(0,11,0,0)));
      check(Native.down==downs+2&&Native.up==ups+1);
      Keys.releaseAll();check(Native.up==ups+2);
    }
    int downs=Native.down,ups=Native.up;
    check(Keys.forward(new KeyEvent(0,11,0,0)));
    check(Keys.forward(new KeyEvent(0,11,0,1))); // no repeat to Lingo
    check(Keys.forward(new KeyEvent(0,12,0,0))); // same key, second keyboard
    check(Native.down==downs+1);
    Keys.releaseDevice(11);check(Native.up==ups);
    Keys.releaseDevice(12);check(Native.up==ups+1);
    // A held key on pause is released once; the next real press is fresh.
    Keys.forward(new KeyEvent(0,11,0,0));Keys.releaseAll();Keys.releaseAll();
    check(Native.up==ups+2);
    check(!Keys.forward(new KeyEvent(1,11,0,0)));
    Keys.forward(new KeyEvent(0,11,0,0));check(Native.down==downs+3);
    Keys.releaseAll();
    check(Keys.forward(new KeyEvent(0,11,1,0)));Keys.releaseAll();
    check(Native.events.stream().allMatch(e -> e.equals("0:97:true") || e.equals("0:97:false")));
    // Physical modifier states and the shifted printable key are all delivered.
    Native.events.clear();
    check(Keys.forward(new KeyEvent(0,11,2,0,KeyEvent.KEYCODE_CTRL_LEFT)));
    check(Keys.forward(new KeyEvent(0,11,10,0,KeyEvent.KEYCODE_SHIFT_LEFT)));
    check(Keys.forward(new KeyEvent(0,11,10,0)));
    check(Keys.forward(new KeyEvent(1,11,0,0)));
    check(Native.events.contains("59:0:true") && Native.events.contains("56:0:true"));
    check(Native.events.contains("0:65:true") && Native.events.contains("0:65:false"));
    Keys.releaseAll();
    check(Native.events.contains("59:0:false") && Native.events.contains("56:0:false"));
    System.out.println("Android keys: modifiers, repeat, shared ownership, pause and disconnect pass");
  }
}
