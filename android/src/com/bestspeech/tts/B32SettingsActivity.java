package com.bestspeech.tts;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Engine settings, reached from the gear beside BestSpeech in
 * Settings &rarr; Text-to-speech output. Declared via {@code android:settingsActivity}
 * on the {@code <tts-engine>} resource.
 *
 * <p>Deliberately covers only what the system UI cannot: which of the 14 voices is
 * the default, and this engine's own pitch and gain. Speech rate stays with the
 * system slider, because Android already applies it per request.
 *
 * <p>The UI is built in code rather than from a layout resource so the offline APK
 * build needs nothing but a single XML file.
 */
public class B32SettingsActivity extends Activity {

    static final String PREFS = "b32tts";
    static final String PREF_VOICE = "voice";
    static final String PREF_PITCH = "pitch";     // percent of the voice's baseline
    static final String PREF_GAIN = "gain";       // engine gain, dB
    /** Bumped on import so the service knows its loaded engine is stale. */
    static final String PREF_DLL_STAMP = "dll_stamp";

    static final String DLL_NAME = "B32_TTS.DLL";
    private static final int REQ_PICK_DLL = 1;

    static final int PITCH_MIN = 50, PITCH_MAX = 200, PITCH_DEFAULT = 100;
    static final int GAIN_MIN = -20, GAIN_MAX = 20, GAIN_DEFAULT = 0;

    private SharedPreferences prefs;
    private TextToSpeech preview;
    private float density;
    private TextView status;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        density = getResources().getDisplayMetrics().density;
        setTitle("BestSpeech");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);

        root.addView(heading("BestSpeech"));
        root.addView(body("A 1995 formant synthesizer (Berkeley Speech "
                + "Technologies) running under CPU emulation."));

        status = body("");
        root.addView(status);

        Button pick = new Button(this);
        pick.setText("Choose " + DLL_NAME + " file...");
        pick.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pickDll(); }
        });
        root.addView(pick);
        root.addView(hint("The speech engine is proprietary and cannot be shipped "
                + "with this app, so pick your own copy. It is copied into this "
                + "app's private storage, so the original can be moved or deleted "
                + "afterwards."));
        refreshStatus();

        // ---- default voice
        root.addView(label("Default voice"));
        final String[] voices = new String[B32Native.voiceCount()];
        for (int i = 0; i < voices.length; i++) voices[i] = B32Native.voiceName(i);
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(
                this, android.R.layout.simple_spinner_item, voices);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        String current = prefs.getString(PREF_VOICE, voices.length > 0 ? voices[0] : "");
        for (int i = 0; i < voices.length; i++) {
            if (voices[i].equals(current)) spinner.setSelection(i);
        }
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                prefs.edit().putString(PREF_VOICE, voices[pos]).apply();
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });
        root.addView(spinner);
        root.addView(hint("Used when the app requesting speech does not name a "
                + "voice itself. Each voice has its own pitch and character."));

        // ---- pitch
        root.addView(slider(root, "Pitch", PREF_PITCH, PITCH_MIN, PITCH_MAX,
                PITCH_DEFAULT, "%"));
        root.addView(hint("Scales the selected voice's baseline frequency."));

        // ---- gain
        root.addView(slider(root, "Volume", PREF_GAIN, GAIN_MIN, GAIN_MAX,
                GAIN_DEFAULT, " dB"));

        // ---- preview
        Button play = new Button(this);
        play.setText("Speak a sample");
        play.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { speakSample(); }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(16);
        root.addView(play, lp);

        root.addView(hint("Speech rate is the system slider on the previous "
                + "screen. This engine cannot exceed about 2.7x normal speed, so "
                + "higher settings stop making a difference."));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (status != null) refreshStatus();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (preview != null) {
            preview.shutdown();
            preview = null;
        }
    }

    // ------------------------------------------------------------------ widgets

    private int dp(int v) { return Math.round(v * density); }

    private TextView text(String s, int sp, boolean bold, int topDp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (bold) t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        t.setPadding(0, dp(topDp), 0, 0);
        return t;
    }

    private TextView heading(String s) { return text(s, 24, true, 0); }
    private TextView body(String s) { return text(s, 14, false, 4); }
    private TextView label(String s) { return text(s, 16, true, 24); }

    private TextView hint(String s) {
        TextView t = text(s, 12, false, 4);
        t.setAlpha(0.7f);
        return t;
    }

    /** Where an imported engine lives: private storage, needing no permission. */
    static File importedDll(android.content.Context c) {
        return new File(c.getFilesDir(), DLL_NAME);
    }

    private void refreshStatus() {
        File internal = importedDll(this);
        File ext = getExternalFilesDir(null) == null ? null
                : new File(getExternalFilesDir(null), DLL_NAME);
        File found = internal.isFile() ? internal
                : (ext != null && ext.isFile() ? ext : null);
        if (found != null) {
            status.setText("Engine ready: " + B32Native.voiceCount()
                    + " voices, loaded from " + found.getName() + " ("
                    + (found.length() / 1024) + " KB).");
            status.setTextColor(Color.rgb(0x1F, 0x7A, 0x4D));
        } else {
            status.setText("No speech engine yet \u2014 choose a " + DLL_NAME
                    + " file below to get started.");
            status.setTextColor(Color.rgb(0xA8, 0x3A, 0x18));
        }
    }

    private void pickDll() {
        // SAF grants access to exactly one file the user chose, so this needs no
        // storage permission and works with Downloads, Drive, USB, anywhere.
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, REQ_PICK_DLL);
        } catch (RuntimeException e) {
            fail("No file picker is available on this device.");
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req != REQ_PICK_DLL || result != RESULT_OK || data == null
                || data.getData() == null) {
            return;
        }
        // Stage to a temp file first: a half-copied engine left in place would
        // fail to load on every subsequent start.
        File dest = importedDll(this);
        File tmp = new File(getFilesDir(), DLL_NAME + ".part");
        long copied = 0;
        try (InputStream in = getContentResolver().openInputStream(data.getData());
             OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException("cannot open the chosen file");
            byte[] buf = new byte[64 * 1024];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                copied += n;
                if (copied > 8L * 1024 * 1024) {
                    throw new IOException("far too large to be " + DLL_NAME);
                }
            }
        } catch (IOException | SecurityException e) {
            tmp.delete();
            fail("Could not read that file: " + e.getMessage());
            return;
        }

        String why = describeIfNotEngine(tmp);
        if (why != null) {
            tmp.delete();
            fail(why);
            return;
        }
        dest.delete();
        if (!tmp.renameTo(dest)) {
            tmp.delete();
            fail("Could not save the engine into private storage.");
            return;
        }
        // Marks the service's loaded copy stale so it reopens the engine without
        // having to be force-stopped.
        prefs.edit().putLong(PREF_DLL_STAMP, System.currentTimeMillis()).apply();
        refreshStatus();
    }

    private void fail(String message) {
        status.setText(message);
        status.setTextColor(Color.rgb(0xA8, 0x3A, 0x18));
    }

    /**
     * Cheap sanity check on a picked file, so choosing the wrong one says so
     * plainly instead of failing obscurely at synthesis time.
     *
     * @return null if it looks like the engine, otherwise why it does not
     */
    private static String describeIfNotEngine(File f) {
        byte[] head = new byte[0x400];
        int n;
        try (InputStream in = new FileInputStream(f)) {
            n = in.read(head);
        } catch (IOException e) {
            return "Could not inspect that file.";
        }
        if (n < 0x40 || head[0] != 'M' || head[1] != 'Z') {
            return "That is not a Windows DLL.";
        }
        int pe = (head[0x3C] & 0xFF) | ((head[0x3D] & 0xFF) << 8);
        if (pe + 6 > n || head[pe] != 'P' || head[pe + 1] != 'E') {
            return "That is not a Windows DLL.";
        }
        int machine = (head[pe + 4] & 0xFF) | ((head[pe + 5] & 0xFF) << 8);
        if (machine != 0x14C) {
            return "That DLL is not 32-bit x86, which this engine has to be.";
        }
        // Checking for the engine's own export rejects unrelated 32-bit DLLs.
        return fileContains(f, "bstCreate") ? null
                : "That DLL is not the BestSpeech engine: no bstCreate export.";
    }

    private static boolean fileContains(File f, String needle) {
        byte[] want = needle.getBytes();
        try (InputStream in = new FileInputStream(f)) {
            int match = 0;
            byte[] buf = new byte[64 * 1024];
            for (int n; (n = in.read(buf)) > 0; ) {
                for (int i = 0; i < n; i++) {
                    match = (buf[i] == want[match]) ? match + 1 : 0;
                    if (match == want.length) return true;
                }
            }
        } catch (IOException e) {
            return false;
        }
        return false;
    }

    /** A labelled SeekBar bound to an int preference, saved as the user drags. */
    private View slider(ViewGroup parent, final String name, final String key,
                        final int min, final int max, int def, final String unit) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);

        final int initial = prefs.getInt(key, def);
        final TextView caption = label(name + ": " + initial + unit);
        box.addView(caption);

        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar b, int progress, boolean fromUser) {
                int value = min + progress;
                caption.setText(name + ": " + value + unit);
                prefs.edit().putInt(key, value).apply();
            }
            @Override public void onStartTrackingTouch(SeekBar b) { }
            @Override public void onStopTrackingTouch(SeekBar b) { }
        });
        box.addView(bar);
        return box;
    }

    private void speakSample() {
        final String sample = "The quick brown fox jumps over the lazy dog.";
        if (preview != null) {
            preview.stop();
            doSpeak(sample);
            return;
        }
        preview = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int status) {
                if (status == TextToSpeech.SUCCESS) doSpeak(sample);
            }
        }, getPackageName());
    }

    private void doSpeak(String sample) {
        // Leaving rate alone means the preview matches what other apps will hear.
        preview.speak(sample, TextToSpeech.QUEUE_FLUSH, null, "preview");
    }
}
