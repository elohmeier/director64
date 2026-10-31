package org.director64.tv;
import android.content.Context;
import android.view.PointerIcon;
public final class CursorSurfaceTest {
  public static void main(String[] args) {
    CursorSurface surface=new CursorSurface(new Context());
    int[] kinds={0,1,2,3,4,200,-1,777};
    int[] icons={PointerIcon.TYPE_ARROW,PointerIcon.TYPE_TEXT,PointerIcon.TYPE_CROSSHAIR,
      PointerIcon.TYPE_CROSSHAIR,PointerIcon.TYPE_WAIT,PointerIcon.TYPE_NULL,
      PointerIcon.TYPE_NULL,PointerIcon.TYPE_NULL};
    for(int i=0;i<kinds.length;i++) {
      surface.apply(kinds[i]);
      if(surface.pointer==null||surface.pointer.type!=icons[i]
          ||surface.onResolvePointerIcon(null,0)!=surface.pointer)throw new AssertionError();
    }
    System.out.println("Android cursor roles: native icons, explicit TYPE_NULL, authored/hidden fallback pass");
  }
}
