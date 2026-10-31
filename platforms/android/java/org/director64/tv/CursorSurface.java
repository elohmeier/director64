package org.director64.tv;

import android.content.Context;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.SurfaceView;

/** Android draws standard mouse icons smoothly; authored cursors stay on stage. */
final class CursorSurface extends SurfaceView {
  private PointerIcon icon;
  int kind = -1;
  int iconType = PointerIcon.TYPE_NULL;

  CursorSurface(Context context) {
    super(context);
    apply(-1);
  }

  void apply(int value) {
    kind = value;
    switch (value) {
      case 0: iconType = PointerIcon.TYPE_ARROW; break;
      case 1: iconType = PointerIcon.TYPE_TEXT; break;
      case 2:
      case 3: iconType = PointerIcon.TYPE_CROSSHAIR; break;
      case 4: iconType = PointerIcon.TYPE_WAIT; break;
      default: iconType = PointerIcon.TYPE_NULL; break;
    }
    icon = PointerIcon.getSystemIcon(getContext(), iconType);
    setPointerIcon(icon);
  }

  @Override public PointerIcon onResolvePointerIcon(MotionEvent event, int pointerIndex) {
    return icon; // null means default, so hiding uses the explicit TYPE_NULL icon.
  }
}
