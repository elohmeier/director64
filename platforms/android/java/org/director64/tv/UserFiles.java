package org.director64.tv;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintJob;
import android.print.PrintManager;
import android.print.pdf.PrintedPdfDocument;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.util.Base64;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONArray;
import org.json.JSONObject;

/** Player-selected document capabilities. Only this activity ever sees their Android URIs. */
final class UserFiles implements Closeable {
  static final int MAX_RAW = FileData.MAX_RAW;
  private static final int PICK_DOCUMENT = 7601, CREATE_DOCUMENT = 7602;
  private final Activity activity;
  private final Handler main = new Handler(Looper.getMainLooper());
  private final ExecutorService io = Executors.newSingleThreadExecutor();
  private final Map<Integer, Uri> destinations = new HashMap<>();
  private Request current;
  private AlertDialog errorDialog;
  private AlertDialog loadingDialog;
  private PrintJob printJob;
  private Bitmap printBitmap;
  private WebView printView;
  private JSONObject printIndex;
  private volatile boolean closed;
  private volatile Closeable stream;

  private static final class Request {
    final int id, kind;
    final String title, name, filter;
    final boolean legacy;
    volatile boolean finished;
    Request(int id, int kind, String title, String name, String filter, boolean legacy) {
      this.id = id; this.kind = kind; this.title = title; this.name = name;
      this.filter = filter; this.legacy = legacy;
    }
  }

  UserFiles(Activity activity) { this.activity = activity; }

  int requestId() { return current == null ? 0 : current.id; }
  int requestKind() { return current == null ? 0 : current.kind; }

  private boolean active(Request request) {
    return !closed && current == request && !request.finished;
  }

  void request(int id, int kind, byte[] title, byte[] name, byte[] filter, byte[] data) {
    if (closed) return;
    if (current != null) {
      if (current.id == id && current.kind == kind) return;
      abandon(current);
    }
    Request request = new Request(id, kind, new String(title, StandardCharsets.UTF_8),
        new String(name, StandardCharsets.UTF_8), new String(filter, StandardCharsets.UTF_8), false);
    current = request;
    if (kind == 1 || kind == 2) choose(request);
    else if (kind == 3) export(request, data);
    else if (kind == 4) print(request, data);
    else fail(request, "The game requested an unsupported document operation.", false);
  }

  private static String basename(String name) {
    int slash = Math.max(name.lastIndexOf('/'), Math.max(name.lastIndexOf('\\'), name.lastIndexOf(':')));
    return name.substring(slash + 1);
  }

  private static boolean imageFilter(String filter) {
    return filter.toLowerCase(java.util.Locale.ROOT).matches(".*\\.(bmp|dib|png|jpg|jpeg|gif|webp).*" );
  }

  private void choose(Request request) {
    if (!active(request)) return;
    Intent intent = new Intent(request.kind == 1 ? Intent.ACTION_OPEN_DOCUMENT : Intent.ACTION_CREATE_DOCUMENT);
    intent.addCategory(Intent.CATEGORY_OPENABLE);
    intent.setType(imageFilter(request.filter) ? "image/*" : "*/*");
    if (request.kind == 2) {
      intent.setType("application/octet-stream");
      String name = basename(request.name);
      if (!name.isEmpty()) intent.putExtra(Intent.EXTRA_TITLE, name);
    }
    try {
      activity.startActivityForResult(intent, request.kind == 1 ? PICK_DOCUMENT : CREATE_DOCUMENT);
    } catch (RuntimeException error) {
      fail(request, "Android could not open its document picker: " + error.getMessage(), false);
    }
  }

  boolean result(int requestCode, int resultCode, Intent result) {
    if (requestCode != PICK_DOCUMENT && requestCode != CREATE_DOCUMENT) return false;
    Request request = current;
    if (request == null || !active(request)) return true;
    if ((requestCode == PICK_DOCUMENT) != (request.kind == 1)) return true;
    if (resultCode != Activity.RESULT_OK || result == null || result.getData() == null) {
      abandon(request);
      return true;
    }
    Uri uri = result.getData();
    loading(request, request.kind == 1 ? "Opening selected file…" : "Selecting destination…");
    io.execute(() -> {
      try {
        String name = displayName(uri);
        byte[] encoded = FileData.filename(name);
        if (!active(request)) return;
        byte[] raw = request.kind == 1 ? FileData.normalizeCar(name, read(uri, request)) : new byte[0];
        if (!active(request)) return;
        byte[] rgba = null;
        int width = 0, height = 0;
        if (request.kind == 1) {
          BitmapFactory.Options bounds = new BitmapFactory.Options();
          bounds.inJustDecodeBounds = true;
          BitmapFactory.decodeByteArray(raw, 0, raw.length, bounds);
          if (bounds.outWidth > 0 && bounds.outHeight > 0) {
            width = bounds.outWidth; height = bounds.outHeight;
            if (!imageFits(width, height))
              throw new IOException("The image must be at most 4096 pixels per side and 8 million pixels in total.");
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            options.inScaled = false;
            options.inPremultiplied = false;
            Bitmap bitmap = BitmapFactory.decodeByteArray(raw, 0, raw.length, options);
            if (bitmap == null) throw new IOException("Android could not decode this image.");
            try {
              if (bitmap.getWidth() != width || bitmap.getHeight() != height)
                throw new IOException("The image dimensions changed during decoding.");
              int[] pixels = new int[width * height];
              bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
              rgba = new byte[pixels.length * 4];
              for (int i = 0; i < pixels.length; i++) {
                int color = pixels[i], at = i * 4;
                rgba[at] = (byte)(color >> 16); rgba[at + 1] = (byte)(color >> 8);
                rgba[at + 2] = (byte)color; rgba[at + 3] = (byte)(color >>> 24);
              }
            } finally { bitmap.recycle(); }
          } else if (imageFilter(request.filter)) {
            throw new IOException("Android could not decode this image. Choose another image or cancel.");
          }
        }
        byte[] pixels = rgba;
        int w = width, h = height;
        main.post(() -> {
          if (!active(request)) return;
          if (request.kind == 2) {
            if (destinations.size() >= 256 && !destinations.containsKey(request.id)) {
              fail(request, "This session has reached its selected-file limit. Cancel and restart the game to select more files.", false);
              return;
            }
            destinations.put(request.id, uri);
          }
          if (!Native.fileReply(request.id, request.kind, encoded, raw, pixels, w, h))
            fail(request, "The game could not accept this file. Choose another file or cancel.", true);
        });
      } catch (IOException | RuntimeException | OutOfMemoryError error) {
        main.post(() -> fail(request, error instanceof OutOfMemoryError
            ? "There is not enough memory to open this file. Choose a smaller file or cancel."
            : error.getMessage(), true));
      }
    });
    return true;
  }

  static boolean imageFits(int width, int height) {
    return FileData.imageFits(width, height);
  }

  private String displayName(Uri uri) throws IOException {
    try (Cursor cursor = activity.getContentResolver().query(uri,
        new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) return cursor.getString(0);
    }
    throw new IOException("Android did not provide a file name. Choose another file or cancel.");
  }

  private byte[] read(Uri uri, Request request) throws IOException {
    try (InputStream input = activity.getContentResolver().openInputStream(uri)) {
      if (input == null) throw new IOException("Android could not open the selected file.");
      stream = input;
      ByteArrayOutputStream output = new ByteArrayOutputStream();
      byte[] block = new byte[32768];
      int count;
      while ((count = input.read(block)) != -1) {
        if (!active(request)) throw new IOException("File selection was cancelled.");
        if (count > MAX_RAW - output.size()) throw new IOException("Choose a file no larger than 16 MiB.");
        output.write(block, 0, count);
      }
      return output.toByteArray();
    } finally { stream = null; }
  }

  void replyResult(int id, int kind, boolean accepted) {
    Request request = current;
    if (request == null || request.id != id || request.kind != kind || !active(request)) return;
    if (!accepted) fail(request, "The game could not import this file. Choose another file or cancel.", true);
    else { dismissLoading(); request.finished = true; current = null; }
  }

  private void export(Request request, byte[] data) {
    Uri uri = destinations.get(request.id);
    if (uri == null || data == null || data.length > MAX_RAW) {
      fail(request, "The game has no selected destination for this file.", false);
      return;
    }
    loading(request, "Saving selected file…");
    io.execute(() -> {
      try {
        byte[] encoded = FileData.exportCar(request.name, data);
        try (OutputStream output = activity.getContentResolver().openOutputStream(uri, "wt")) {
          if (output == null) throw new IOException("Android could not write the selected file.");
          stream = output;
          if (!active(request)) throw new IOException("The save was cancelled.");
          output.write(encoded); output.flush();
        }
        main.post(() -> complete(request, true));
      } catch (IOException | RuntimeException error) {
        main.post(() -> fail(request, "The file could not be saved: " + error.getMessage(), false));
      } finally { stream = null; }
    });
  }

  private void print(Request request, byte[] data) {
    if (data == null || data.length > MAX_RAW) {
      fail(request, "The game supplied an invalid print document.", false); return;
    }
    BitmapFactory.Options bounds = new BitmapFactory.Options();
    bounds.inJustDecodeBounds = true;
    BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
    if (!imageFits(bounds.outWidth, bounds.outHeight)) {
      fail(request, "The print image exceeds the supported image size.", false); return;
    }
    try {
      Bitmap bitmap = BitmapFactory.decodeByteArray(data, 0, data.length);
      if (bitmap == null) throw new IllegalArgumentException("Android could not decode the print image.");
      printBitmap(request, bitmap);
    } catch (RuntimeException | OutOfMemoryError error) {
      fail(request, "Android could not prepare the print image.", false);
    }
  }

  private byte[] printAsset(String name) throws Exception {
    if (printIndex == null) {
      try (InputStream input = activity.getAssets().open("game/print.json")) {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] block = new byte[4096];
        int count;
        while ((count = input.read(block)) != -1) {
          if (count > MAX_RAW - output.size()) throw new IOException("Print index exceeds its size limit.");
          output.write(block, 0, count);
        }
        JSONObject index = new JSONObject(new String(output.toByteArray(), StandardCharsets.UTF_8));
        if (index.getInt("version") != 1 || !index.getString("blob").equals("print"))
          throw new IOException("The APK contains an unsupported print pack.");
        printIndex = index.getJSONObject("entries");
      }
    }
    JSONArray entry = printIndex.getJSONArray(name);
    long offset = entry.getLong(0), size = entry.getLong(1);
    if (offset < 0 || size < 0 || size > MAX_RAW || offset > Long.MAX_VALUE - size)
      throw new IOException("The APK contains an invalid print asset range.");
    byte[] bytes = new byte[(int)size];
    try (InputStream input = activity.getAssets().open("game/print.bin")) {
      while (offset > 0) {
        long skipped = input.skip(offset);
        if (skipped == 0) {
          if (input.read() == -1) throw new IOException("The APK print pack is truncated.");
          skipped = 1;
        }
        offset -= skipped;
      }
      int at = 0;
      while (at < bytes.length) {
        int count = input.read(bytes, at, bytes.length - at);
        if (count < 0) throw new IOException("The APK print asset is truncated.");
        at += count;
      }
    }
    return bytes;
  }

  private String printImage(String name) throws Exception {
    return "data:image/png;base64," + Base64.encodeToString(printAsset(name), Base64.NO_WRAP);
  }

  void legacyPrint(int id) {
    if (closed) return;
    Request request = new Request(id, 4, "Print document", "game-document", "", true);
    if (current != null) abandon(current);
    current = request;
    io.execute(() -> {
      try {
        JSONArray documents = new JSONArray(new String(printAsset("documents.json"), StandardCharsets.UTF_8));
        JSONObject document = documents.getJSONObject(id - 1);
        String title = document.getString("title");
        StringBuilder html = new StringBuilder("<!doctype html><html lang='de'><meta charset='utf-8'><style>")
            .append("@page{size:A4;margin:50pt 50pt 40pt 60pt}html,body{margin:0;background:#fff;color:#000}")
            .append("body{font:12pt/16pt Arial,Helvetica,sans-serif;position:relative}.logo{position:absolute;top:0;right:0}")
            .append("h1{margin:58pt 0 14pt;font-size:18pt;line-height:22pt}p{margin:0 0 12pt;break-inside:avoid}")
            .append(".illustration{display:block;margin-top:8pt;width:451pt;height:356pt;break-inside:avoid}")
            .append("footer{position:fixed;bottom:0;right:0;font-size:9pt}</style><title>")
            .append(TextUtils.htmlEncode(title)).append("</title><body><img class='logo' style='width:")
            .append(document.getInt("logo_width")).append("pt' src='").append(printImage(document.getString("logo")))
            .append("'><h1>").append(TextUtils.htmlEncode(title)).append("</h1>");
        for (String paragraph : document.getString("body").split("\r\r", -1))
          html.append("<p>").append(TextUtils.htmlEncode(paragraph).replace("\r", "<br>")).append("</p>");
        if (!document.isNull("illustration"))
          html.append("<img class='illustration' style='margin-left:").append(document.getInt("illustration_x"))
              .append("pt' src='").append(printImage(document.getString("illustration"))).append("'>");
        html.append("<footer>© Bavaria Film Multimedia</footer></body></html>");
        main.post(() -> { if (active(request)) printHtml(request, title, html.toString()); });
      } catch (Exception | OutOfMemoryError error) {
        main.post(() -> fail(request, "The authored print document could not be loaded: " + error.getMessage(), false));
      }
    });
  }

  private void printHtml(Request request, String title, String html) {
    try {
      WebView view = new WebView(activity);
      printView = view;
      view.getSettings().setAllowFileAccess(false);
      view.getSettings().setAllowContentAccess(false);
      view.getSettings().setBlockNetworkLoads(true);
      view.setWebViewClient(new WebViewClient() {
        private boolean launched;
        @Override public void onPageFinished(WebView loaded, String url) {
          if (launched || !active(request)) return;
          launched = true;
          PrintManager manager = (PrintManager)activity.getSystemService(Activity.PRINT_SERVICE);
          if (manager == null) { fail(request, "Android printing is unavailable.", false); view.destroy(); return; }
          PrintDocumentAdapter delegate = view.createPrintDocumentAdapter(title);
          PrintDocumentAdapter adapter = new PrintDocumentAdapter() {
            @Override public void onStart() { delegate.onStart(); }
            @Override public void onLayout(PrintAttributes oldAttributes, PrintAttributes newAttributes,
                CancellationSignal cancelled, LayoutResultCallback callback, android.os.Bundle extras) {
              if (!active(request) || cancelled.isCanceled()) { callback.onLayoutCancelled(); return; }
              delegate.onLayout(oldAttributes, newAttributes, cancelled, callback, extras);
            }
            @Override public void onWrite(PageRange[] pages, ParcelFileDescriptor destination,
                CancellationSignal cancelled, WriteResultCallback callback) {
              if (!active(request) || cancelled.isCanceled()) { callback.onWriteCancelled(); return; }
              delegate.onWrite(pages, destination, cancelled, callback);
            }
            @Override public void onFinish() {
              delegate.onFinish();
              boolean accepted = printJob != null && !printJob.isCancelled() && !printJob.isFailed()
                  && printJob.getInfo().getState() != android.print.PrintJobInfo.STATE_CREATED;
              if (active(request)) complete(request, accepted);
              if (printView == view) { view.destroy(); printView = null; }
              printJob = null;
            }
          };
          try {
            printJob = manager.print(title, adapter,
                new PrintAttributes.Builder().setMediaSize(PrintAttributes.MediaSize.ISO_A4).build());
          } catch (RuntimeException error) {
            view.destroy(); printView = null;
            fail(request, "Android could not open the print dialog: " + error.getMessage(), false);
          }
        }
      });
      view.loadDataWithBaseURL(null, html, "text/html", "utf-8", null);
    } catch (RuntimeException | OutOfMemoryError error) {
      fail(request, "Android could not prepare the authored print document.", false);
    }
  }

  private void printBitmap(Request request, Bitmap bitmap) {
    printBitmap = bitmap;
    PrintManager manager = (PrintManager)activity.getSystemService(Activity.PRINT_SERVICE);
    if (manager == null) { bitmap.recycle(); printBitmap = null; fail(request, "Android printing is unavailable.", false); return; }
    PrintDocumentAdapter adapter = new PrintDocumentAdapter() {
      private PrintAttributes attributes;
      private boolean wrote;
      @Override public void onLayout(PrintAttributes oldAttributes, PrintAttributes newAttributes,
          CancellationSignal cancelled, LayoutResultCallback callback, android.os.Bundle extras) {
        if (cancelled.isCanceled() || !active(request)) { callback.onLayoutCancelled(); return; }
        attributes = newAttributes;
        callback.onLayoutFinished(new PrintDocumentInfo.Builder("game-picture.pdf")
            .setContentType(PrintDocumentInfo.CONTENT_TYPE_PHOTO).setPageCount(1).build(), true);
      }
      @Override public void onWrite(PageRange[] pages, ParcelFileDescriptor destination,
          CancellationSignal cancelled, WriteResultCallback callback) {
        if (cancelled.isCanceled() || !active(request)) { callback.onWriteCancelled(); return; }
        boolean pageRequested = false;
        for (PageRange page : pages) if (page.getStart() <= 0 && page.getEnd() >= 0) pageRequested = true;
        if (!pageRequested) { callback.onWriteFinished(new PageRange[0]); return; }
        if (attributes == null) { callback.onWriteFailed("Android supplied no page layout."); return; }
        PrintedPdfDocument document = new PrintedPdfDocument(activity, attributes);
        try (FileOutputStream output = new FileOutputStream(destination.getFileDescriptor())) {
          android.graphics.pdf.PdfDocument.Page page = document.startPage(0);
          try {
            Rect area = page.getInfo().getContentRect();
            Canvas canvas = page.getCanvas();
            float scale = Math.min((float)area.width() / bitmap.getWidth(), (float)area.height() / bitmap.getHeight());
            float w = bitmap.getWidth() * scale, h = bitmap.getHeight() * scale;
            float x = area.left + (area.width() - w) / 2, y = area.top + (area.height() - h) / 2;
            canvas.drawBitmap(bitmap, null, new RectF(x, y, x + w, y + h), new Paint(Paint.FILTER_BITMAP_FLAG));
          } finally { document.finishPage(page); }
          if (cancelled.isCanceled()) { callback.onWriteCancelled(); return; }
          document.writeTo(output);
          wrote = true;
          callback.onWriteFinished(new PageRange[] {PageRange.ALL_PAGES});
        } catch (IOException | RuntimeException | OutOfMemoryError error) { callback.onWriteFailed("Android could not prepare the PDF."); }
        finally { document.close(); }
      }
      @Override public void onFinish() {
        boolean accepted = wrote && printJob != null && !printJob.isCancelled() && !printJob.isFailed();
        if (active(request)) complete(request, accepted);
        if (!bitmap.isRecycled()) bitmap.recycle();
        if (printBitmap == bitmap) printBitmap = null;
        printJob = null;
      }
    };
    try {
      printJob = manager.print(request.title.isEmpty() ? "Game picture" : request.title, adapter,
          new PrintAttributes.Builder().setMediaSize(bitmap.getWidth() > bitmap.getHeight()
              ? PrintAttributes.MediaSize.ISO_A4.asLandscape() : PrintAttributes.MediaSize.ISO_A4.asPortrait()).build());
    } catch (RuntimeException error) {
      bitmap.recycle(); printBitmap = null;
      fail(request, "Android could not open the print dialog: " + error.getMessage(), false);
    }
  }

  private void fail(Request request, String message, boolean retry) {
    if (!active(request)) return;
    dismissLoading();
    if (errorDialog != null) errorDialog.dismiss();
    AlertDialog.Builder builder = new AlertDialog.Builder(activity).setTitle("File operation")
        .setMessage(message == null ? "Android could not complete this operation." : message)
        .setNegativeButton(android.R.string.cancel, (dialog, which) -> abandon(request))
        .setOnCancelListener(dialog -> abandon(request));
    if (retry && (request.kind == 1 || request.kind == 2))
      builder.setPositiveButton("Choose again", (dialog, which) -> choose(request));
    errorDialog = builder.show();
  }

  private void complete(Request request, boolean success) {
    if (!active(request)) return;
    dismissLoading();
    request.finished = true; current = null;
    Native.fileComplete(request.id, request.kind, success);
  }

  private void abandon(Request request) {
    if (!active(request)) return;
    dismissLoading();
    request.finished = true; current = null;
    Closeable activeStream = stream;
    if (activeStream != null) try { activeStream.close(); } catch (IOException ignored) {}
    if (request.kind == 3 || request.kind == 4) Native.fileComplete(request.id, request.kind, false);
    else Native.fileCancel(request.id, request.kind);
  }

  private void loading(Request request, String message) {
    dismissLoading();
    loadingDialog = new AlertDialog.Builder(activity).setMessage(message)
        .setNegativeButton(android.R.string.cancel, (dialog, which) -> abandon(request))
        .setOnCancelListener(dialog -> abandon(request)).show();
  }

  private void dismissLoading() {
    if (loadingDialog != null) { loadingDialog.dismiss(); loadingDialog = null; }
  }

  @Override public void close() {
    if (closed) return;
    if (current != null) abandon(current);
    closed = true;
    if (errorDialog != null) errorDialog.dismiss();
    if (printJob != null && !printJob.isCompleted() && !printJob.isCancelled()) printJob.cancel();
    Closeable activeStream = stream;
    if (activeStream != null) try { activeStream.close(); } catch (IOException ignored) {}
    io.shutdownNow(); destinations.clear();
    if (printView != null) { printView.stopLoading(); printView.destroy(); printView = null; }
    if (printBitmap != null) { printBitmap.recycle(); printBitmap = null; }
    main.removeCallbacksAndMessages(null);
  }
}
