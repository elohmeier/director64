package android.view;
public final class MotionEvent {
  public static final int ACTION_MOVE=2,AXIS_X=0,AXIS_Y=1,AXIS_Z=2,AXIS_RZ=3,
      AXIS_HAT_X=4,AXIS_HAT_Y=5;
  private final InputDevice device;
  private final float x,y;
  public MotionEvent(InputDevice device,float x,float y){this.device=device;this.x=x;this.y=y;}
  public InputDevice getDevice(){return device;}
  public int getSource(){return InputDevice.SOURCE_JOYSTICK;}
  public int getAction(){return ACTION_MOVE;}
  public long getEventTime(){return 1000;}
  public float getAxisValue(int axis){return axis==AXIS_X?x:axis==AXIS_Y?y:0;}
}
