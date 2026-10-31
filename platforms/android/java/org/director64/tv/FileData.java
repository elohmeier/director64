package org.director64.tv;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** Limits and legacy encodings shared by document import and its source-free tests. */
final class FileData {
  static final int MAX_RAW = 16 * 1024 * 1024, MAX_RGBA = 32 * 1024 * 1024;
  private static final Charset WINDOWS_1252 = Charset.forName("windows-1252");

  private FileData() {}

  static boolean imageFits(int width, int height) {
    return width > 0 && height > 0 && width <= 4096 && height <= 4096
        && (long)width * height * 4 <= MAX_RGBA;
  }

  static byte[] filename(String name) throws IOException {
    byte[] encoded;
    try { encoded = encode(name, StandardCharsets.UTF_8); }
    catch (CharacterCodingException error) { throw new IOException("The selected file name is not valid Unicode."); }
    if (encoded.length == 0 || encoded.length >= 128 || name.equals(".") || name.equals("..")
        || name.chars().anyMatch(c -> c < 32 || c == 127 || c == ':' || c == '/' || c == '\\'))
      throw new IOException("Choose a file with a simple name shorter than 128 UTF-8 bytes.");
    return encoded;
  }

  private static byte[] encode(CharSequence text, Charset charset) throws CharacterCodingException {
    ByteBuffer encoded = charset.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(text));
    byte[] result = new byte[encoded.remaining()];
    encoded.get(result); return result;
  }

  private static boolean raster(byte[] b) {
    return (b.length >= 8 && b[0] == (byte)137 && b[1] == 80 && b[2] == 78 && b[3] == 71
        && b[4] == 13 && b[5] == 10 && b[6] == 26 && b[7] == 10)
        || (b.length >= 3 && b[0] == (byte)255 && b[1] == (byte)216 && b[2] == (byte)255)
        || (b.length >= 2 && b[0] == 'B' && b[1] == 'M')
        || (b.length >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8'
            && (b[4] == '7' || b[4] == '9') && b[5] == 'a')
        || (b.length >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F'
            && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P');
  }

  private static byte[] bounded(byte[] bytes) throws IOException {
    if (bytes.length > MAX_RAW) throw new IOException("The UTF-8 CAR text exceeds 16 MiB. Choose a smaller file or cancel.");
    return bytes;
  }

  static byte[] normalizeCar(String name, byte[] raw) throws IOException {
    if (!name.toLowerCase(Locale.ROOT).endsWith(".car") || raster(raw)) return raw;
    int start = raw.length >= 3 && raw[0] == (byte)0xef && raw[1] == (byte)0xbb
        && raw[2] == (byte)0xbf ? 3 : 0;
    try {
      CharBuffer text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
          .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw, start, raw.length - start));
      return bounded(encode(text, StandardCharsets.UTF_8));
    } catch (CharacterCodingException legacy) {
      try { return bounded(encode(WINDOWS_1252.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
          .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw, start, raw.length - start)),
          StandardCharsets.UTF_8)); }
      catch (CharacterCodingException invalid) { throw new IOException("The CAR file has an invalid text encoding."); }
    }
  }

  static byte[] exportCar(String name, byte[] raw) throws IOException {
    if (!name.toLowerCase(Locale.ROOT).endsWith(".car") || raster(raw)) return raw;
    try {
      CharBuffer text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
          .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw));
      return encode(text, WINDOWS_1252);
    } catch (CharacterCodingException error) {
      throw new IOException("This CAR file contains text that the original Windows game cannot save.");
    }
  }
}
