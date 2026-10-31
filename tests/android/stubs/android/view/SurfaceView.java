package android.view;
import android.content.Context;
public class SurfaceView {
  private final Context context;
  public PointerIcon pointer;
  public SurfaceView(Context context){this.context=context;}
  public Context getContext(){return context;}
  public void setPointerIcon(PointerIcon pointer){this.pointer=pointer;}
  public PointerIcon onResolvePointerIcon(MotionEvent event,int pointerIndex){return null;}
}
