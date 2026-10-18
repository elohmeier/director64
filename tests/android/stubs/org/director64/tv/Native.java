package org.director64.tv;
final class Native {
  static int down, up;
  static void key(int code,int character,boolean pressed) {
    if(code!=0||character!=97)throw new AssertionError("changed key identity");
    if(pressed)down++;else up++;
  }
}
