package com.brruham.luaaml;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Map;

public final class Shell {
    private static final String TAG = "libluaaml-ui";

    static native byte[] nativeRun(byte[] code);
    static native void nativeStop();
    static native void nativeReset();
    static native byte[] nativeInfo();

    private static Activity cur;
    private static View toggle;
    private static LinearLayout panel;
    private static EditText input;
    private static TextView output, title;
    private static ScrollView scroll;
    private static volatile boolean running = false;
    private static String lastResult = "";
    private static final StringBuilder log = new StringBuilder();
    private static final ArrayList<String> hist = new ArrayList<>();
    private static int hpos = 0;
    private static float density = 1f;
    private static final Handler ui = new Handler(Looper.getMainLooper());

    private static int dp(int v) { return (int) (v * density + 0.5f); }

    public static boolean boot(Activity act) {
        try {
            if (act == null) act = findActivity();
            if (act == null) return false;
            if (act == cur && toggle != null && toggle.isAttachedToWindow()) return true;
            final Activity a = act;
            a.runOnUiThread(() -> {
                try { attach(a); } catch (Throwable t) { Log.e(TAG, "attach", t); }
            });
            return act == cur && toggle != null && toggle.isAttachedToWindow();
        } catch (Throwable t) {
            Log.e(TAG, "boot", t);
            return false;
        }
    }

    private static Activity findActivity() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            Field f = at.getDeclaredField("mActivities");
            f.setAccessible(true);
            Object m = f.get(thread);
            if (!(m instanceof Map)) return null;
            for (Object rec : ((Map<?, ?>) m).values()) {
                Class<?> rc = rec.getClass();
                Field pf = rc.getDeclaredField("paused");
                pf.setAccessible(true);
                if (pf.getBoolean(rec)) continue;
                Field af = rc.getDeclaredField("activity");
                af.setAccessible(true);
                return (Activity) af.get(rec);
            }
        } catch (Throwable t) {
            Log.w(TAG, "findActivity: " + t);
        }
        return null;
    }

    private static void detach() {
        try {
            if (toggle != null && toggle.getParent() instanceof ViewGroup)
                ((ViewGroup) toggle.getParent()).removeView(toggle);
            if (panel != null && panel.getParent() instanceof ViewGroup)
                ((ViewGroup) panel.getParent()).removeView(panel);
        } catch (Throwable ignored) { }
    }

    private static Button btn(Context c, String t, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(t);
        b.setAllCaps(false);
        b.setTextSize(11);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setPadding(dp(2), dp(5), dp(2), dp(5));
        b.setOnClickListener(l);
        b.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return b;
    }

    private static LinearLayout row(Context c) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return r;
    }

    private static void attach(Activity a) {
        detach();
        cur = a;
        DisplayMetrics dm = a.getResources().getDisplayMetrics();
        density = dm.density;
        try {
            a.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        } catch (Throwable ignored) { }
        ViewGroup root = (ViewGroup) a.findViewById(android.R.id.content);
        if (root == null) root = (ViewGroup) a.getWindow().getDecorView();

        int pw = Math.min((int) (dm.widthPixels * 0.94f), dp(640));
        int ph = (int) (dm.heightPixels * 0.55f);

        panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xEE12181F);
        bg.setCornerRadius(dp(8));
        bg.setStroke(dp(1), 0xFF3A4654);
        panel.setBackground(bg);
        panel.setPadding(dp(6), dp(6), dp(6), dp(6));
        panel.setVisibility(View.GONE);

        title = new TextView(a);
        title.setTextColor(0xFF8FB4D9);
        title.setTextSize(10);
        try {
            title.setText(new String(nativeInfo(), StandardCharsets.UTF_8));
        } catch (Throwable t) {
            title.setText("libluaaml");
        }
        panel.addView(title);

        input = new EditText(a);
        input.setTypeface(Typeface.MONOSPACE);
        input.setTextSize(13);
        input.setTextColor(Color.WHITE);
        input.setHintTextColor(0xFF7A8794);
        input.setHint("kode Lua... (contoh: 1+1 atau print(ctx.base))");
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        input.setMinLines(2);
        input.setMaxLines(4);
        input.setGravity(Gravity.TOP | Gravity.START);
        input.setBackgroundColor(0xFF0B0F14);
        input.setPadding(dp(6), dp(4), dp(6), dp(4));
        panel.addView(input, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout r1 = row(a);
        r1.addView(btn(a, "Run", v -> doRun()));
        r1.addView(btn(a, "Stop", v -> nativeStop()));
        r1.addView(btn(a, "Clr out", v -> { log.setLength(0); output.setText(""); }));
        r1.addView(btn(a, "Clr in", v -> input.setText("")));
        r1.addView(btn(a, "Hide", v -> togglePanel()));
        panel.addView(r1);

        LinearLayout r2 = row(a);
        r2.addView(btn(a, "Paste", v -> paste()));
        r2.addView(btn(a, "Copy out", v -> copy(log.toString())));
        r2.addView(btn(a, "Copy hasil", v -> copy(lastResult)));
        r2.addView(btn(a, "\u25B2", v -> histMove(-1)));
        r2.addView(btn(a, "\u25BC", v -> histMove(1)));
        r2.addView(btn(a, "Reset", v -> doReset()));
        panel.addView(r2);

        output = new TextView(a);
        output.setTypeface(Typeface.MONOSPACE);
        output.setTextSize(11);
        output.setTextColor(0xFFB8F5C4);
        output.setTextIsSelectable(true);
        output.setPadding(dp(4), dp(4), dp(4), dp(4));
        output.setText(log.toString());
        scroll = new ScrollView(a);
        scroll.setBackgroundColor(0xFF0B0F14);
        scroll.addView(output);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        slp.topMargin = dp(4);
        panel.addView(scroll, slp);

        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(pw, ph,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        plp.topMargin = dp(4);
        root.addView(panel, plp);

        TextView t = new TextView(a);
        t.setText("Lua");
        t.setTextColor(Color.WHITE);
        t.setTextSize(11);
        t.setGravity(Gravity.CENTER);
        GradientDrawable tb = new GradientDrawable();
        tb.setShape(GradientDrawable.OVAL);
        tb.setColor(0xCC1E6FD9);
        t.setBackground(tb);
        final float[] st = new float[4];
        final boolean[] moved = { false };
        t.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    st[0] = e.getRawX(); st[1] = e.getRawY();
                    st[2] = v.getTranslationX(); st[3] = v.getTranslationY();
                    moved[0] = false;
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    float dx = e.getRawX() - st[0], dy = e.getRawY() - st[1];
                    if (Math.abs(dx) > dp(6) || Math.abs(dy) > dp(6)) moved[0] = true;
                    if (moved[0]) { v.setTranslationX(st[2] + dx); v.setTranslationY(st[3] + dy); }
                    return true;
                }
                case MotionEvent.ACTION_UP:
                    if (!moved[0]) togglePanel();
                    return true;
                default:
                    return false;
            }
        });
        toggle = t;
        FrameLayout.LayoutParams tlp = new FrameLayout.LayoutParams(dp(42), dp(42),
                Gravity.TOP | Gravity.END);
        tlp.setMargins(0, dp(8), dp(8), 0);
        root.addView(t, tlp);
        Log.i(TAG, "UI terpasang");
    }

    private static void togglePanel() {
        if (panel == null) return;
        if (panel.getVisibility() == View.VISIBLE) {
            hideKb();
            panel.setVisibility(View.GONE);
        } else {
            panel.setVisibility(View.VISIBLE);
            panel.bringToFront();
            if (toggle != null) toggle.bringToFront();
        }
    }

    private static void hideKb() {
        try {
            InputMethodManager imm = (InputMethodManager) cur.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null && input != null) imm.hideSoftInputFromWindow(input.getWindowToken(), 0);
        } catch (Throwable ignored) { }
    }

    private static void toast(String s) {
        try { Toast.makeText(cur, s, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) { }
    }

    private static void append(String s) {
        log.append(s);
        if (!s.endsWith("\n")) log.append('\n');
        if (log.length() > 30000) log.delete(0, log.length() - 20000);
        output.setText(log.toString());
        scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN));
    }

    private static void doRun() {
        if (running) { toast("masih berjalan, tekan Stop"); return; }
        final String code = input.getText().toString();
        if (code.trim().isEmpty()) return;
        if (hist.isEmpty() || !hist.get(hist.size() - 1).equals(code)) hist.add(code);
        hpos = hist.size();
        append("> " + code.replace("\n", "\n  "));
        running = true;
        new Thread(() -> {
            String out;
            try {
                out = new String(nativeRun(code.getBytes(StandardCharsets.UTF_8)), StandardCharsets.UTF_8);
            } catch (Throwable t) {
                out = "! " + t;
            }
            final String o = out;
            ui.post(() -> {
                lastResult = o;
                append(o.isEmpty() ? "(ok)" : o);
                running = false;
            });
        }, "luaaml-run").start();
    }

    private static void doReset() {
        nativeStop();
        new Thread(() -> {
            nativeReset();
            ui.post(() -> append("(state Lua direset)"));
        }, "luaaml-reset").start();
    }

    private static void histMove(int d) {
        if (hist.isEmpty()) return;
        hpos = Math.max(0, Math.min(hist.size(), hpos + d));
        if (hpos == hist.size()) {
            input.setText("");
        } else {
            input.setText(hist.get(hpos));
            input.setSelection(input.getText().length());
        }
    }

    private static void paste() {
        try {
            ClipboardManager cm = (ClipboardManager) cur.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null || !cm.hasPrimaryClip()) { toast("clipboard kosong"); return; }
            ClipData cd = cm.getPrimaryClip();
            if (cd == null || cd.getItemCount() == 0) { toast("clipboard kosong"); return; }
            CharSequence t = cd.getItemAt(0).coerceToText(cur);
            if (t == null) { toast("clipboard kosong"); return; }
            int s = Math.max(input.getSelectionStart(), 0);
            int e = Math.max(input.getSelectionEnd(), 0);
            input.getText().replace(Math.min(s, e), Math.max(s, e), t);
        } catch (Throwable t) {
            toast("paste gagal: " + t);
        }
    }

    private static void copy(String text) {
        try {
            ClipboardManager cm = (ClipboardManager) cur.getSystemService(Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("luaaml", text));
            toast("disalin (" + text.length() + " karakter)");
        } catch (Throwable t) {
            toast("copy gagal: " + t);
        }
    }
}
