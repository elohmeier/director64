package org.director64.tv;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.charset.Charset;
import java.util.regex.Pattern;
import org.json.JSONObject;

/**
 * Plays the APK's game: the native runtime draws the 640x480 stage into a 4:3 surface and runs at
 * the console's service clock (platforms/android/jni/main.c); this activity feeds it the
 * controllers, remote, mouse and keyboard, and shows the menu and text entry. The game's package
 * and asset packs ship as the APK's assets (assets/game/), its profile as assets/game/profile.json.
 */
public final class GameActivity extends Activity implements InputManager.InputDeviceListener {
  private static final int WIDTH = 640, HEIGHT = 480;
  private static final Charset WINDOWS_1252 = Charset.forName("windows-1252");

  private final Pads pads = new Pads();
  private final Handler handler = new Handler(Looper.getMainLooper());
  private Text text;
  private SurfaceView stage;
  private TextView overlay;
  private SharedPreferences preferences;
  private InputManager inputs;
  private boolean started, stopped, showDiagnostics, keyboardGame, german;
  private AlertDialog dialog, keyEntry;
  private int mouseButtons;
  private String title;

  /** The stage, letterboxed to 4:3 on the screen. */
  private static final class Stage extends FrameLayout {
    final SurfaceView surface;

    Stage(Context context, SurfaceView surface) {
      super(context);
      this.surface = surface;
      addView(surface);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
      int width = r - l, height = b - t;
      int w = Math.min(width, height * 4 / 3), h = w * 3 / 4;
      int x = (width - w) / 2, y = (height - h) / 2;
      surface.layout(x, y, x + w, y + h);
      for (int i = 1; i < getChildCount(); i++) {
        View child = getChildAt(i);
        child.layout(x, y, x + child.getMeasuredWidth(), y + child.getMeasuredHeight());
      }
    }
  }

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    preferences = getSharedPreferences("player", MODE_PRIVATE);
    JSONObject profile;
    try (InputStream in = getAssets().open("game/profile.json")) {
      profile = new JSONObject(new String(Text.readAll(in), StandardCharsets.UTF_8));
    } catch (Exception error) {
      throw new IllegalStateException("this APK holds no game profile", error);
    }
    title = profile.optString("title");
    int version = profile.optInt("director_version");
    // From Director 7 on, scripts receive keys; ten takes German letters.
    keyboardGame = version >= 700;
    german = version >= 1000;
    System.loadLibrary(profile.optString("library"));
    text = new Text(getAssets(), new File(getCacheDir(), "fonts"));

    stage = new SurfaceView(this);
    stage.getHolder().addCallback(
        new SurfaceHolder.Callback() {
          @Override
          public void surfaceCreated(SurfaceHolder holder) {}

          @Override
          public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            Native.surface(holder.getSurface());
          }

          @Override
          public void surfaceDestroyed(SurfaceHolder holder) {
            Native.surface(null);
          }
        });
    stage.setOnTouchListener((view, event) -> mouse(view, event));
    stage.setOnGenericMotionListener((view, event) -> mouse(view, event));
    Stage root = new Stage(this, stage);
    root.setBackgroundColor(Color.BLACK);
    overlay = new TextView(this);
    overlay.setTextColor(Color.WHITE);
    overlay.setBackgroundColor(0x99000000);
    overlay.setTextSize(12);
    overlay.setPadding(12, 8, 12, 8);
    overlay.setVisibility(View.GONE);
    overlay.setLayoutParams(
        new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    root.addView(overlay);
    setContentView(root);
    immersive();

    inputs = (InputManager) getSystemService(INPUT_SERVICE);
    inputs.registerInputDeviceListener(this, handler);
    // The console's FlashRAM image, in the app's own storage.
    Native.start(
        this,
        getAssets(),
        new File(getFilesDir(), "flash.bin").getAbsolutePath(),
        preferences.getFloat("volume", 0.8f));
    started = true;
    handler.postDelayed(this::tick, 1000);
  }

  private void immersive() {
    getWindow()
        .getDecorView()
        .setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
  }

  // Once a second: the diagnostics line and idle remotes.
  private void tick() {
    if (stopped) return;
    pads.expire(android.os.SystemClock.uptimeMillis());
    if (showDiagnostics) {
      overlay.setText(Native.diagnostics() + "\n" + pads.describe() + pads.lastInput);
      overlay.measure(0, 0);
      overlay.requestLayout();
    }
    handler.postDelayed(this::tick, 1000);
  }

  @Override
  protected void onResume() {
    super.onResume();
    immersive();
    if (started) Native.hold(Native.HOLD_PAUSED, false);
  }

  @Override
  protected void onPause() {
    if (started) Native.hold(Native.HOLD_PAUSED, true);
    super.onPause();
  }

  @Override
  protected void onDestroy() {
    stopped = true;
    handler.removeCallbacksAndMessages(null);
    if (inputs != null) inputs.unregisterInputDeviceListener(this);
    if (started) Native.stop();
    super.onDestroy();
    // The runtime's state lives for the life of the process; leaving the game
    // ends it, so it starts fresh next time.
    if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
  }

  // ---- Input ----

  private static boolean opensMenu(int keyCode) {
    return keyCode == KeyEvent.KEYCODE_BACK
        || keyCode == KeyEvent.KEYCODE_MENU
        || keyCode == KeyEvent.KEYCODE_BUTTON_MODE
        || keyCode == KeyEvent.KEYCODE_BUTTON_SELECT
        || keyCode == KeyEvent.KEYCODE_ESCAPE;
  }

  @Override
  public boolean dispatchKeyEvent(KeyEvent event) {
    if (dialog != null && dialog.isShowing()) return super.dispatchKeyEvent(event);
    int code = event.getKeyCode();
    if (code == KeyEvent.KEYCODE_VOLUME_UP
        || code == KeyEvent.KEYCODE_VOLUME_DOWN
        || code == KeyEvent.KEYCODE_VOLUME_MUTE
        || code == KeyEvent.KEYCODE_HOME) return super.dispatchKeyEvent(event);
    InputDevice device = event.getDevice();
    // A real keyboard types into games that read keys (Escape included);
    // the remote's Back still opens the menu.
    if (keyboardGame && Pads.isKeyboard(device) && Keys.forward(event)) return true;
    if (opensMenu(code)) {
      if (event.getAction() == KeyEvent.ACTION_UP) showMenu();
      return true;
    }
    if (pads.key(event)) return true;
    return super.dispatchKeyEvent(event);
  }

  @Override
  public boolean dispatchGenericMotionEvent(MotionEvent event) {
    if (dialog != null && dialog.isShowing()) return super.dispatchGenericMotionEvent(event);
    if (pads.motion(event)) return true;
    return super.dispatchGenericMotionEvent(event);
  }

  // A mouse over the stage, in 640x480 units the runtime maps to its stage;
  // a touch is a click.
  private boolean mouse(View view, MotionEvent event) {
    int x = (int) (event.getX() * WIDTH / view.getWidth());
    int y = (int) (event.getY() * HEIGHT / view.getHeight());
    int action = event.getActionMasked();
    if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
      int state = event.getButtonState();
      mouseButtons =
          ((state & MotionEvent.BUTTON_PRIMARY) != 0 ? Pads.A : 0)
              | ((state & MotionEvent.BUTTON_SECONDARY) != 0 ? Pads.B : 0);
      if (action == MotionEvent.ACTION_DOWN && mouseButtons == 0) mouseButtons = Pads.A;
      if (action == MotionEvent.ACTION_UP) mouseButtons = 0;
    } else {
      mouseButtons =
          action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE ? Pads.A : 0;
    }
    boolean inside = action != MotionEvent.ACTION_HOVER_EXIT;
    Native.mouse(x, y, mouseButtons, inside);
    return true;
  }

  @Override
  public void onInputDeviceAdded(int deviceId) {}

  @Override
  public void onInputDeviceRemoved(int deviceId) {
    pads.removed(deviceId);
  }

  @Override
  public void onInputDeviceChanged(int deviceId) {}

  // ---- Dialogs: each holds the game while it is open ----

  private void opened(AlertDialog shown, int hold) {
    dialog = shown;
    pads.release();
    Native.hold(hold, true);
  }

  private void showMenu() {
    if (stopped || (dialog != null && dialog.isShowing())) return;
    int percent = Math.round(preferences.getFloat("volume", 0.8f) * 100);
    String[] items = {
      getString(R.string.menu_resume),
      getString(R.string.menu_louder, percent),
      getString(R.string.menu_quieter, percent),
      getString(showDiagnostics ? R.string.menu_hide_diagnostics : R.string.menu_diagnostics),
      getString(R.string.menu_controls),
      getString(R.string.menu_quit),
    };
    AlertDialog menu =
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(
                items,
                (d, which) -> {
                  switch (which) {
                    case 1:
                    case 2:
                      float level = preferences.getFloat("volume", 0.8f) + (which == 1 ? 0.1f : -0.1f);
                      level = Math.max(0, Math.min(1, Math.round(level * 10) / 10f));
                      preferences.edit().putFloat("volume", level).apply();
                      Native.volume(level);
                      handler.post(this::showMenu);
                      break;
                    case 3:
                      showDiagnostics = !showDiagnostics;
                      overlay.setVisibility(showDiagnostics ? View.VISIBLE : View.GONE);
                      break;
                    case 4:
                      handler.post(this::showControls);
                      break;
                    case 5:
                      finish();
                      break;
                    default:
                      break;
                  }
                })
            .create();
    menu.setOnDismissListener(d -> Native.hold(Native.HOLD_MENU, false));
    opened(menu, Native.HOLD_MENU);
    menu.show();
  }

  private void showControls() {
    AlertDialog controls =
        new AlertDialog.Builder(this)
            .setTitle(R.string.menu_controls)
            .setMessage(getString(R.string.controls_text) + "\n\n" + pads.describe())
            .setPositiveButton(android.R.string.ok, null)
            .create();
    controls.setOnDismissListener(d -> Native.hold(Native.HOLD_MENU, false));
    opened(controls, Native.HOLD_MENU);
    controls.show();
  }

  // ---- Called by the game thread ----

  /** Each character's box in a text box (Text.layout). */
  short[] layout(
      byte[] utf8, int width, int height, int font, int size, int align, int ascent, int lineHeight) {
    return text.layout(utf8, width, height, font, size, align, ascent, lineHeight);
  }

  /** A text box's coverage plane (Text.rasterize). */
  byte[] rasterize(
      byte[] utf8, int width, int height, int font, int size, int align, int ascent, int lineHeight) {
    return text.rasterize(utf8, width, height, font, size, align, ascent, lineHeight);
  }

  /**
   * Text entry over an editable field, as the console's on-screen keyboard. Field text is
   * windows-1252: printable ASCII except the double quote everywhere, German letters on Director
   * 10, at most 20 characters. An edit (keys false) holds the game until it is committed or
   * cancelled. A key field (Mucklas' name) instead takes each character as the game's key presses
   * while the game runs, so its own handlers check and sound them; OK closes the keyboard, and
   * the game closes it once it is done with the field.
   */
  void openTextEntry(
      int sprite, byte[] value, int left, int top, int right, int bottom, boolean keys) {
    String current = new String(value, WINDOWS_1252);
    handler.post(
        () -> {
          if (stopped) return;
          Pattern allowed =
              Pattern.compile(german ? "^[\\x20-\\x21\\x23-\\x7eÄÖÜßäöü]*$" : "^[\\x20-\\x21\\x23-\\x7e]*$");
          EditText field = new EditText(this);
          field.setSingleLine(true);
          field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
          // The dialog stays in view above the keyboard.
          field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
          InputFilter length = new InputFilter.LengthFilter(20);
          // The console keyboard's characters (platforms/n64/text_input.c).
          InputFilter typable =
              (source, start, end, dest, dstart, dend) -> {
                StringBuilder kept = new StringBuilder();
                for (int i = start; i < end; i++) {
                  char c = source.charAt(i);
                  if (c < 128 && (Character.isLetterOrDigit(c) || " -_.".indexOf(c) >= 0)) kept.append(c);
                }
                return kept.length() == end - start ? null : kept;
              };
          field.setFilters(keys ? new InputFilter[] {length, typable} : new InputFilter[] {length});
          field.setText(current);
          field.setSelection(current.length());
          AlertDialog.Builder builder =
              new AlertDialog.Builder(this)
                  .setTitle(R.string.text_title)
                  .setView(field)
                  .setPositiveButton(android.R.string.ok, null);
          if (!keys) builder.setNegativeButton(android.R.string.cancel, (d, w) -> Native.cancelText());
          AlertDialog entry = builder.create();
          entry.setOnCancelListener(d -> Native.cancelText());
          Runnable commit;
          if (keys) {
            // Each edit reaches the game as the keys that make it: deletes
            // back to where the text changed, then the new characters.
            field.addTextChangedListener(
                new TextWatcher() {
                  String typed = current;

                  @Override
                  public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

                  @Override
                  public void onTextChanged(CharSequence s, int start, int before, int count) {}

                  @Override
                  public void afterTextChanged(Editable s) {
                    String now = s.toString();
                    int same = 0;
                    while (same < typed.length() && same < now.length()
                        && typed.charAt(same) == now.charAt(same)) same++;
                    for (int i = same; i < typed.length(); i++) Native.typeKey(8);
                    for (int i = same; i < now.length(); i++) Native.typeKey(now.charAt(i));
                    typed = now;
                  }
                });
            entry.setOnDismissListener(d -> Native.cancelText());
            commit = entry::dismiss;
            keyEntry = entry;
          } else {
            commit =
                () -> {
                  String typed = field.getText().toString();
                  if (!allowed.matcher(typed).matches() || typed.length() > 20) {
                    field.setError(getString(R.string.text_bad_character));
                  } else if (Native.commitText(typed.getBytes(WINDOWS_1252))) {
                    entry.dismiss();
                  } else {
                    field.setError(getString(R.string.text_refused));
                  }
                };
          }
          field.setOnEditorActionListener(
              (v, action, event) -> {
                commit.run();
                return true;
              });
          entry.setOnShowListener(
              d -> {
                entry.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> commit.run());
                field.requestFocus();
                entry.getWindow()
                    .setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
              });
          if (dialog != null && dialog.isShowing()) dialog.dismiss();
          dialog = entry;
          pads.release();
          entry.show();
        });
  }

  /** The game is done with its key field: the keyboard closes. */
  void closeTextEntry() {
    handler.post(
        () -> {
          if (keyEntry != null && keyEntry.isShowing()) keyEntry.dismiss();
          keyEntry = null;
        });
  }

  void gameStopped(int status, byte[] message) {
    String error = new String(message, WINDOWS_1252);
    handler.post(
        () -> {
          if (isFinishing()) return;
          if (dialog != null && dialog.isShowing()) dialog.dismiss();
          AlertDialog.Builder builder =
              new AlertDialog.Builder(this)
                  .setTitle(status == 2 ? R.string.game_ended : R.string.game_stopped)
                  .setCancelable(false)
                  .setPositiveButton(R.string.close, (d, w) -> finish());
          if (status != 2) builder.setMessage(error + "\n\n" + getString(R.string.saves_kept));
          dialog = builder.create();
          dialog.show();
        });
  }

  void printRequested(int document) {
    handler.post(() -> Toast.makeText(this, R.string.no_printing, Toast.LENGTH_LONG).show());
  }

  void saveCommitted(boolean ok) {
    if (!ok)
      handler.post(() -> Toast.makeText(this, R.string.save_failed, Toast.LENGTH_LONG).show());
  }
}
