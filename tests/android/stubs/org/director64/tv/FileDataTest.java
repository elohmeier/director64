package org.director64.tv;

import java.io.IOException;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class FileDataTest {
  private static void require(boolean value) {
    if (!value) throw new AssertionError();
  }
  public static void main(String[] args) throws Exception {
    require(FileData.imageFits(4096, 2048));
    require(!FileData.imageFits(4096, 2049));
    require(!FileData.imageFits(4097, 1));
    require(!FileData.imageFits(Integer.MAX_VALUE, Integer.MAX_VALUE));
    require(!FileData.imageFits(0, 1) && !FileData.imageFits(1, -1));
    Charset legacy = Charset.forName("windows-1252");
    String german = "[#name:\"Müller Straße €\"]";
    byte[] car = german.getBytes(legacy), utf8 = german.getBytes(StandardCharsets.UTF_8);
    require(Arrays.equals(FileData.normalizeCar("my.CAR", utf8), utf8));
    require(Arrays.equals(FileData.normalizeCar("legacy.car", car), utf8));
    require(Arrays.equals(FileData.exportCar("legacy.car", utf8), car));
    require(Arrays.equals(FileData.normalizeCar("roundtrip.car", FileData.exportCar("roundtrip.car", utf8)), utf8));
    byte[] bom = {(byte)0xef, (byte)0xbb, (byte)0xbf, 'x'};
    require(Arrays.equals(FileData.normalizeCar("bom.car", bom), new byte[] {'x'}));
    byte[] unknownUnicode = "🚙".getBytes(StandardCharsets.UTF_8);
    require(Arrays.equals(FileData.normalizeCar("unknown.car", unknownUnicode), unknownUnicode));
    try { FileData.exportCar("unknown.car", unknownUnicode); throw new AssertionError(); }
    catch (IOException expected) {}
    byte[] binary = {0, (byte)0xff, 1, 2, 3};
    require(FileData.normalizeCar("picture.png", binary) == binary);
    require(FileData.exportCar("picture.png", binary) == binary);
    byte[] misnamedPng = {(byte)137,80,78,71,13,10,26,10,0,(byte)255};
    require(FileData.normalizeCar("misnamed.car", misnamedPng) == misnamedPng);
    require(FileData.exportCar("misnamed.car", misnamedPng) == misnamedPng);
    require(FileData.normalizeCar("text.txt", bom) == bom);
    require(Arrays.equals(FileData.filename("Müller.car"), "Müller.car".getBytes(StandardCharsets.UTF_8)));
    require(Arrays.equals(FileData.filename("🚙.car"), "🚙.car".getBytes(StandardCharsets.UTF_8)));
    for (String name : new String[] {"", ".", "..", "a/b", "a\\b", "C:a", "a\0b", "a\nb", "\ud800.car", "x".repeat(128), "ü".repeat(64)}) {
      try { FileData.filename(name); throw new AssertionError("accepted " + name); }
      catch (IOException expected) {}
    }
    System.out.println("Android files: image allocation bounds, exact binary bytes, strict CAR encodings and names pass");
  }
}
