/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

/** The PC viewer's look (tools/raw-recv/ui.h): its theme, DejaVu Sans, and the
 *  pill buttons, segmented choices, switches and status ring it draws. */
final class Ui {
    static final int BG = 0xFF0C0E12, SURF = 0xFF15181E, SURF2 = 0xFF1F232B, SURF3 = 0xFF2C313B,
            INK = 0xFFECEEF3, DIM = 0xFF98A0AF, FAINT = 0xFF646C7A,
            ACC = 0xFF38BDF8, GOOD = 0xFF34D399, WARN = 0xFFFBBF24, BAD = 0xFFF87171,
            HUD = 0xAA0A0C10;
    static Typeface regular = Typeface.DEFAULT, bold = Typeface.DEFAULT_BOLD;
    private static float density = 1;

    static void init(Context context) {
        density = context.getResources().getDisplayMetrics().density;
        try {
            regular = Typeface.createFromAsset(context.getAssets(), "fonts/DejaVuSans.ttf");
            bold = Typeface.createFromAsset(context.getAssets(), "fonts/DejaVuSans-Bold.ttf");
        } catch (RuntimeException e) { regular = Typeface.DEFAULT; bold = Typeface.DEFAULT_BOLD; }
    }
    static int dp(float value) { return Math.round(value * density); }

    static TextView text(Context c, String value, float sp, int color, boolean heavy) {
        TextView t = new TextView(c);
        t.setText(value); t.setTextSize(sp); t.setTextColor(color);
        t.setTypeface(heavy ? bold : regular); t.setIncludeFontPadding(false);
        return t;
    }
    static GradientDrawable round(int fill, float radiusDp) {
        GradientDrawable d = new GradientDrawable(); d.setColor(fill); d.setCornerRadius(dp(radiusDp)); return d;
    }
    static GradientDrawable round(int fill, float radiusDp, int stroke) {
        GradientDrawable d = round(fill, radiusDp); d.setStroke(dp(2), stroke); return d;
    }
    /** Focus (remote) and press (touch) both show the accent ring. */
    static StateListDrawable states(int fill, int focusFill, float radiusDp) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, round(focusFill, radiusDp, ACC));
        s.addState(new int[]{android.R.attr.state_pressed}, round(focusFill, radiusDp, ACC));
        s.addState(new int[]{}, round(fill, radiusDp));
        return s;
    }
    /** A pill button: SURF2, or the accent with dark text for the main action. */
    static Button pill(Context c, String label, boolean primary, View.OnClickListener click) {
        Button b = new Button(c);
        b.setText(label); b.setAllCaps(false); b.setTextSize(15); b.setTypeface(bold);
        b.setMinHeight(0); b.setMinimumHeight(0); b.setMinWidth(0); b.setMinimumWidth(0);
        b.setPadding(dp(22), dp(10), dp(22), dp(10));
        b.setStateListAnimator(null);
        stylePill(b, primary);
        b.setOnClickListener(click);
        return b;
    }
    static void stylePill(Button b, boolean primary) {
        b.setTextColor(primary ? BG : INK);
        b.setBackground(primary ? states(ACC, 0xFF7DD3FC, 22) : states(SURF2, SURF3, 22));
    }
    /** One choice of a segmented control (USB / Network). */
    static void styleSegment(Button b, boolean selected) {
        b.setTextColor(selected ? BG : INK);
        StateListDrawable s = new StateListDrawable();
        int fill = selected ? ACC : 0x00000000;
        s.addState(new int[]{android.R.attr.state_focused}, round(selected ? ACC : SURF3, 8, INK));
        s.addState(new int[]{android.R.attr.state_pressed}, round(selected ? ACC : SURF3, 8, INK));
        s.addState(new int[]{}, round(fill, 8));
        b.setBackground(s);
    }
    /** A settings row: SURF2 behind it while it has the focus, as in the PC drawer. */
    static StateListDrawable rowBackground() {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, round(SURF2, 10));
        s.addState(new int[]{android.R.attr.state_pressed}, round(SURF2, 10));
        s.addState(new int[]{}, round(0x00000000, 10));
        return s;
    }
    /** A key hint: the key in a SURF3 chip, then what it does. */
    static View keyHint(Context c, String key, String what) {
        android.widget.LinearLayout row = new android.widget.LinearLayout(c);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView k = text(c, key, 12, INK, true);
        k.setPadding(dp(7), dp(4), dp(7), dp(4)); k.setBackground(round(SURF3, 5));
        TextView w = text(c, what, 13, DIM, false); w.setPadding(dp(8), 0, dp(22), 0);
        row.addView(k); row.addView(w);
        return row;
    }

    /** The ring with a dot in the middle, the home screen's state. */
    static final class Ring extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int color = ACC;
        Ring(Context c) { super(c); }
        void setColor(int value) { if (value != color) { color = value; invalidate(); } }
        @Override protected void onDraw(Canvas canvas) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f, r = Math.min(cx, cy);
            paint.setColor((color & 0x00FFFFFF) | 0x40000000); canvas.drawCircle(cx, cy, r, paint);
            paint.setColor(color); canvas.drawCircle(cx, cy, r * 0.36f, paint);
        }
    }
    /** A filled circle (the title's dot). */
    static final class Dot extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Dot(Context c, int color) { super(c); paint.setColor(color); }
        @Override protected void onDraw(Canvas canvas) {
            canvas.drawCircle(getWidth() / 2f, getHeight() / 2f, Math.min(getWidth(), getHeight()) / 2f, paint);
        }
    }
    /** The on/off switch: an accent track when on, SURF3 when off, a light knob. */
    static final class Toggle extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF track = new RectF();
        private boolean on;
        Toggle(Context c) { super(c); setFocusable(false); }
        void set(boolean value) { if (value != on) { on = value; invalidate(); } }
        @Override protected void onDraw(Canvas canvas) {
            float w = getWidth(), h = getHeight(), r = h / 2f;
            paint.setColor(on ? ACC : SURF3);
            track.set(0, 0, w, h);
            canvas.drawRoundRect(track, r, r, paint);
            paint.setColor(INK);
            float k = r - dp(3);
            canvas.drawCircle(on ? w - r : r, r, k, paint);
        }
    }
    /** The settings gear, drawn as the PC viewer draws it: teeth around a ring. */
    static final class Gear extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path ring = new Path();
        private final RectF tooth = new RectF();
        Gear(int color) { paint.setColor(color); ring.setFillType(Path.FillType.EVEN_ODD); }
        @Override public void draw(Canvas canvas) {
            android.graphics.Rect b = getBounds();
            float cx = b.exactCenterX(), cy = b.exactCenterY(), s = Math.min(b.width(), b.height()) / 2f;
            tooth.set(cx - s * 0.17f, cy - s, cx + s * 0.17f, cy - s * 0.5f);
            for (int i = 0; i < 8; i++) {
                canvas.save();
                canvas.rotate(i * 45, cx, cy);
                canvas.drawRoundRect(tooth, s * 0.06f, s * 0.06f, paint);
                canvas.restore();
            }
            ring.reset();
            ring.addCircle(cx, cy, s * 0.70f, Path.Direction.CW);
            ring.addCircle(cx, cy, s * 0.30f, Path.Direction.CW);
            canvas.drawPath(ring, paint);
        }
        @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); }
        @Override public void setColorFilter(ColorFilter filter) { paint.setColorFilter(filter); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    private Ui() { }
}
