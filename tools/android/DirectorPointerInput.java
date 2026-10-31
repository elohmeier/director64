import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;

/** Shell-owned real Android hover injection; no app/runtime methods are called. */
public final class DirectorPointerInput {
  public static void main(String[] args) throws Exception {
    if (args.length != 2) throw new IllegalArgumentException("expected screen x y");
    float x=Float.parseFloat(args[0]),y=Float.parseFloat(args[1]);
    if (!Float.isFinite(x)||!Float.isFinite(y)||x<0||y<0||x>16384||y>16384)
      throw new IllegalArgumentException("screen point outside test bounds");
    MotionEvent.PointerProperties property=new MotionEvent.PointerProperties();
    property.id=0;property.toolType=MotionEvent.TOOL_TYPE_MOUSE;
    MotionEvent.PointerCoords coordinates=new MotionEvent.PointerCoords();
    coordinates.x=x;coordinates.y=y;
    long now=SystemClock.uptimeMillis();
    MotionEvent event=MotionEvent.obtain(now,now,MotionEvent.ACTION_HOVER_MOVE,1,
        new MotionEvent.PointerProperties[]{property},new MotionEvent.PointerCoords[]{coordinates},
        0,0,1,1,-1,0,InputDevice.SOURCE_MOUSE,0);
    try {
      // Android14's context-free global service works in a shell process;
      // InputManager.getInstance() requires an ActivityThread application.
      Class<?> type=Class.forName("android.hardware.input.InputManagerGlobal");
      Object manager=type.getDeclaredMethod("getInstance").invoke(null);
      boolean accepted=(Boolean)type.getMethod("injectInputEvent",InputEvent.class,int.class)
          .invoke(manager,event,2);
      if (!accepted) throw new IllegalStateException("Android rejected hover injection");
    } finally {event.recycle();}
  }
}
