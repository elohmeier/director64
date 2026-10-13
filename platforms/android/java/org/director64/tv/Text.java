package org.director64.tv;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Typeface;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Text boxes as coverage planes, set the way the browser player sets them on a canvas
 * (platforms/web/site/player.js textRasterizer). Text without an authored style prints in the
 * console's builtin monospace font, monogram, at the size whose ascent the console's atlas records
 * (11 px, 13 px lines), on or off per pixel. A styled member sets in the game's own font, recovered
 * from the disc or the port's substitute, at its authored size, alignment, ascent and line height.
 */
final class Text {
  private final Map<Integer, Typeface> fonts = new HashMap<>();
  private final Typeface monogram;
  private final float monogramSize;
  private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

  Text(AssetManager assets, File cache) {
    monogram = Typeface.createFromAsset(assets, "monogram.ttf");
    paint.setTypeface(monogram);
    paint.setTextSize(16);
    float ascent16 = -paint.getFontMetrics().ascent;
    monogramSize = Math.max(1, Math.round(16 * 11 / (ascent16 > 0 ? ascent16 : 16)));
    try {
      loadFonts(assets, cache);
    } catch (Exception error) {
      android.util.Log.w("director64", "fonts could not be loaded", error);
    }
  }

  // The fonts pack: each font by the number styles name it with. A typeface
  // loads from a file, so each is written to the cache once.
  private void loadFonts(AssetManager assets, File cache) throws Exception {
    JSONObject entries;
    byte[] blob;
    try (InputStream index = assets.open("game/fonts.json");
        InputStream data = assets.open("game/fonts.bin")) {
      entries = new JSONObject(new String(readAll(index), StandardCharsets.UTF_8));
      blob = readAll(data);
    }
    cache.mkdirs();
    for (Iterator<String> names = entries.keys(); names.hasNext(); ) {
      String name = names.next();
      JSONArray at = entries.getJSONArray(name);
      int offset = at.getInt(0), length = at.getInt(1);
      File file = new File(cache, "font-" + name + ".ttf");
      if (!file.isFile() || file.length() != length) {
        try (FileOutputStream out = new FileOutputStream(file)) {
          out.write(blob, offset, length);
        }
      }
      try {
        fonts.put(Integer.parseInt(name), Typeface.createFromFile(file));
      } catch (RuntimeException error) {
        android.util.Log.w("director64", "font " + name + " could not be loaded", error);
      }
    }
  }

  // Director wraps a text member's paragraphs at its box width, on spaces.
  private List<String> lines(String text, int width) {
    List<String> out = new ArrayList<>();
    for (String paragraph : text.split("\r\n|\r|\n", -1)) {
      StringBuilder line = new StringBuilder();
      for (String word : paragraph.split("(?<= )")) {
        if (line.length() > 0 && paint.measureText(line + stripEnd(word)) > width) {
          out.add(line.toString());
          line.setLength(0);
        }
        line.append(word);
      }
      out.add(line.toString());
    }
    return out;
  }

  /**
   * Each character's box as rasterize sets it: x, y, width, height per UTF-16 unit (player.js
   * layout).
   */
  synchronized short[] layout(
      byte[] utf8, int width, int height, int font, int size, int align, int ascent, int lineHeight) {
    String text = new String(utf8, StandardCharsets.UTF_8);
    short[] boxes = new short[text.length() * 4];
    Typeface face = size != 0 ? fonts.get(font) : null;
    List<int[]> starts = new ArrayList<>();
    List<String> rows = new ArrayList<>();
    int step;
    if (face == null) {
      paint.setTypeface(monogram);
      paint.setTextSize(monogramSize);
      step = 13;
      int at = 0;
      for (String row : text.split("\r\n|\r|\n", -1)) {
        rows.add(row);
        starts.add(new int[] {at});
        at += row.length();
        if (at < text.length()) at += text.startsWith("\r\n", at) ? 2 : 1;
      }
    } else {
      paint.setTypeface(face);
      paint.setTextSize(size);
      step = lineHeight > 0 ? lineHeight : Math.round(size * 1.2f);
      int start = 0;
      while (true) {
        int end = start;
        while (end < text.length() && text.charAt(end) != '\r' && text.charAt(end) != '\n') end++;
        StringBuilder line = new StringBuilder();
        int lineStart = start, at = start;
        for (String word : text.substring(start, end).split("(?<= )")) {
          if (line.length() > 0 && paint.measureText(line + stripEnd(word)) > width) {
            rows.add(line.toString());
            starts.add(new int[] {lineStart});
            line.setLength(0);
            lineStart = at;
          }
          line.append(word);
          at += word.length();
        }
        rows.add(line.toString());
        starts.add(new int[] {lineStart});
        if (end >= text.length()) break;
        start = end + (text.startsWith("\r\n", end) ? 2 : 1);
      }
    }
    for (int row = 0; row < rows.size(); row++) {
      String line = rows.get(row);
      float shown = paint.measureText(stripEnd(line));
      float offset = face == null ? 0 : align == 1 ? (width - shown) / 2 : align == 2 ? width - shown : 0;
      int start = starts.get(row)[0];
      for (int k = 0; k < line.length() && start + k < text.length(); k++) {
        float left = offset + paint.measureText(line, 0, k);
        float right = offset + paint.measureText(line, 0, k + 1);
        int at = (start + k) * 4;
        boxes[at] = (short) Math.round(left);
        boxes[at + 1] = (short) (row * step);
        boxes[at + 2] = (short) Math.max(1, Math.round(right - left));
        boxes[at + 3] = (short) step;
      }
    }
    return boxes;
  }

  private static String stripEnd(String s) {
    int end = s.length();
    while (end > 0 && Character.isWhitespace(s.charAt(end - 1))) end--;
    return s.substring(0, end);
  }

  synchronized byte[] rasterize(
      byte[] utf8, int width, int height, int font, int size, int align, int ascent, int lineHeight) {
    String text = new String(utf8, StandardCharsets.UTF_8);
    Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ALPHA_8);
    Canvas canvas = new Canvas(bitmap);
    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
    paint.setColor(Color.WHITE);
    Typeface face = size != 0 ? fonts.get(font) : null;
    boolean builtin = face == null;
    if (builtin) {
      paint.setTypeface(monogram);
      paint.setTextSize(monogramSize);
      paint.setTextAlign(Paint.Align.LEFT);
      String[] rows = text.split("\r\n|\r|\n", -1);
      for (int i = 0; i < rows.length; i++) canvas.drawText(rows[i], 0, 12 + i * 13, paint);
    } else {
      paint.setTypeface(face);
      paint.setTextSize(size);
      int baseline = ascent > 0 ? ascent : Math.round(size * 0.8f);
      int step = lineHeight > 0 ? lineHeight : Math.round(size * 1.2f);
      paint.setTextAlign(
          align == 1 ? Paint.Align.CENTER : align == 2 ? Paint.Align.RIGHT : Paint.Align.LEFT);
      float x = align == 1 ? width / 2f : align == 2 ? width : 0;
      List<String> rows = lines(text, width);
      for (int i = 0; i < rows.size(); i++)
        canvas.drawText(stripEnd(rows.get(i)), x, baseline + i * step, paint);
    }
    ByteBuffer buffer = ByteBuffer.allocate(bitmap.getRowBytes() * height);
    bitmap.copyPixelsToBuffer(buffer);
    bitmap.recycle();
    byte[] rows = buffer.array();
    byte[] coverage = new byte[width * height];
    int stride = rows.length / height;
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        int alpha = rows[y * stride + x] & 0xff;
        // A pixel font: coverage is on or off, as the console's 1-bit atlas is.
        coverage[y * width + x] = (byte) (builtin ? (alpha >= 128 ? 255 : 0) : alpha);
      }
    }
    return coverage;
  }

  static byte[] readAll(InputStream in) throws IOException {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    byte[] buffer = new byte[65536];
    for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
    return out.toByteArray();
  }
}
