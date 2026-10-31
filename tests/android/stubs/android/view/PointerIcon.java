package android.view;
import android.content.Context;
public final class PointerIcon {
  public static final int TYPE_NULL=0,TYPE_ARROW=1000,TYPE_TEXT=1008,
      TYPE_CROSSHAIR=1007,TYPE_WAIT=1004;
  public final int type;
  private PointerIcon(int type){this.type=type;}
  public static PointerIcon getSystemIcon(Context context,int type){return new PointerIcon(type);}
}
