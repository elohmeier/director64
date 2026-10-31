package org.director64.tv;
final class Native {
  static int down, up;
  static final java.util.List<String> events=new java.util.ArrayList<>();
  static final java.util.List<int[]> pads=new java.util.ArrayList<>();
  static void pad(boolean connected,int buttons,int x,int y) {
    pads.add(new int[]{connected?1:0,buttons,x,y});
  }
  static void key(int code,int character,boolean pressed) {
    events.add(code+":"+character+":"+pressed);
    if(pressed)down++;else up++;
  }
}
