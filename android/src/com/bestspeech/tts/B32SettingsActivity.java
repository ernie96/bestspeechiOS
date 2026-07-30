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
import android.widget.CheckBox;
import android.widget.CompoundButton;
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
 * <p>Covers what the system UI cannot: which of the 14 voices is the default, and
 * the engine's own voice-quality and text-parser parameters. Speech rate stays
 * with the system slider, because Android already applies it per request.
 *
 * <p>The UI is built in code rather than from a layout resource so the offline APK
 * build needs nothing but a single XML file.
 */
public class B32SettingsActivity extends Activity {

    static final String PREFS = "b32tts";
    static final String PREF_VOICE = "voice";
    static final String PREF_PITCH = "pitch";     // percent of the voice's baseline
    static final String PREF_GAIN = "gain";       // engine gain, dB
    // Voice quality; B32Native.VOICE_DEFAULT means "whatever the voice says".
    static final String PREF_INFLECTION = "inflection";
    static final String PREF_HEAD_SIZE = "head_size";
    static final String PREF_EXCITATION = "excitation";
    static final String PREF_UNVOICED = "unvoiced";
    static final String PREF_PARSE_FLAGS = "parse_flags";        // B32Native.PARSE_*
    static final String PREF_PHRASE_PREDICTION = "phrase_pred";
    /** Interior pause cap in ms; 0 trims only the tail, -1 trims nothing. */
    static final String PREF_PAUSE_CAP_MS = "pause_cap_ms";
    /** Bumped on import so the service knows its loaded engine is stale. */
    static final String PREF_DLL_STAMP = "dll_stamp";

    static final String DLL_NAME = "B32_TTS.DLL";
    private static final int REQ_PICK_DLL = 1;

    static final int PITCH_MIN = 50, PITCH_MAX = 200, PITCH_DEFAULT = 100;
    static final int GAIN_MIN = -20, GAIN_MAX = 20, GAIN_DEFAULT = 0;
    static final int PAUSE_DEFAULT = 0;
    /** The engine's 477 ms of trailing silence, cut to something usable. */
    static final int TAIL_CAP_DEFAULT = 60;

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

        // ---- voice quality
        root.addView(heading2("Voice quality"));
        root.addView(hint("The rest of the Keynote GOLD voice parameters. Each "
                + "one left at 'Voice default' keeps whatever the "
                + "chosen voice specifies."));

        root.addView(choice("Head size", PREF_HEAD_SIZE, B32Native.VOICE_DEFAULT,
                names("Voice default", 0, 6, 1, ""), values(0, 6, 1)));
        root.addView(hint("Size of the modelled vocal tract, in no particular "
                + "order. 0 and 1 are the same."));

        root.addView(choice("Excitation", PREF_EXCITATION, B32Native.VOICE_DEFAULT,
                new String[] { "Voice default", "1 - breathy",
                               "2 - whispery", "3 - normal", "4", "5", "6" },
                new int[] { B32Native.VOICE_DEFAULT, 1, 2, 3, 4, 5, 6 }));

        root.addView(choice("Inflection", PREF_INFLECTION, B32Native.VOICE_DEFAULT,
                names("Voice default", -300, 100, 25, ""), values(-300, 100, 25)));
        root.addView(hint("Pitch range. Lower is more monotone, higher more "
                + "expressive. Values below about -150 all sound the same, and a "
                + "high-pitched voice runs out of range at the top."));

        root.addView(choice("Unvoiced volume", PREF_UNVOICED, B32Native.VOICE_DEFAULT,
                names("Voice default", -70, 20, 5, " dB"), values(-70, 20, 5)));
        root.addView(hint("Loudness of unvoiced sounds such as s and f. Turned "
                + "down, the speaker sounds blocked up."));

        // ---- text processing
        root.addView(heading2("Text processing"));

        root.addView(checkbox("Phrase prediction", PREF_PHRASE_PREDICTION, true));
        root.addView(hint("Look ahead for punctuation before committing to a "
                + "phrase, which is what shapes the intonation. Turning it off "
                + "speaks text as it arrives, flatter but a little sooner."));

        root.addView(flag("Expand abbreviations", B32Native.PARSE_ABBREV));
        root.addView(hint("Dr. becomes doctor, St. becomes street."));
        root.addView(flag("Speak times of day", B32Native.PARSE_TIMES));
        root.addView(hint("8:00 becomes eight o'clock."));
        root.addView(flag("Read numbers in full", B32Native.PARSE_FULL_NUMBERS));
        root.addView(hint("1995 as one nine nine five rather than nineteen "
                + "ninety-five."));
        root.addView(flag("Speak digits one at a time", B32Native.PARSE_DIGITS));
        root.addView(flag("Capital groups as words", B32Native.PARSE_UPPER_WORDS));
        root.addView(hint("NASA as a word instead of four letters."));
        root.addView(flag("Spell every letter", B32Native.PARSE_LETTER_NAMES));
        root.addView(flag("Speak punctuation", B32Native.PARSE_PUNCTUATION));
        root.addView(flag("Speak spaces and line breaks", B32Native.PARSE_WHITESPACE));

        // ---- pauses
        root.addView(choice("Pauses", PREF_PAUSE_CAP_MS, PAUSE_DEFAULT,
                new String[] { "Authentic", "Default", "Short", "Shorter", "Shortest" },
                new int[] { -1, 0, 300, 150, 60 }));
        root.addView(hint("The engine pauses about 420 ms at a sentence and "
                + "adds 477 ms after every utterance. Default trims only that "
                + "trailing silence; the shorter settings cap the pauses inside "
                + "the utterance too. Authentic trims nothing."));

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
                + "screen. The engine itself stops at about 2.7x normal speed; "
                + "past that the rest is made up by time-stretching, which keeps "
                + "the pitch but changes the character a little."));

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
    private TextView heading2(String s) { return text(s, 19, true, 32); }
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

    /**
     * Value list for a {@link #choice} covering an inclusive range, prefixed by
     * {@link B32Native#VOICE_DEFAULT}.
     */
    private static int[] values(int from, int to, int step) {
        int n = (to - from) / step + 2;
        int[] out = new int[n];
        out[0] = B32Native.VOICE_DEFAULT;
        for (int i = 1; i < n; i++) out[i] = from + (i - 1) * step;
        return out;
    }

    /** The matching labels, so the two arrays cannot drift apart. */
    private static String[] names(String first, int from, int to, int step,
                                  String unit) {
        int[] v = values(from, to, step);
        String[] out = new String[v.length];
        out[0] = first;
        for (int i = 1; i < v.length; i++) out[i] = v[i] + unit;
        return out;
    }

    /** A labelled Spinner bound to an int preference. */
    private View choice(final String name, final String key, final int def,
                        final String[] labels, final int[] vals) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.addView(label(name));

        Spinner sp = new Spinner(this);
        ArrayAdapter<String> ad = new ArrayAdapter<>(
                this, android.R.layout.simple_spinner_item, labels);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setPrompt(name);
        int cur = prefs.getInt(key, def);
        for (int i = 0; i < vals.length; i++) {
            if (vals[i] == cur) sp.setSelection(i);
        }
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                prefs.edit().putInt(key, vals[pos]).apply();
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });
        box.addView(sp);
        return box;
    }

    /** A CheckBox bound to a boolean preference. */
    private View checkbox(String name, final String key, final boolean def) {
        CheckBox cb = new CheckBox(this);
        cb.setText(name);
        cb.setChecked(prefs.getBoolean(key, def));
        cb.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                prefs.edit().putBoolean(key, on).apply();
            }
        });
        return cb;
    }

    /** A CheckBox bound to one bit of {@link #PREF_PARSE_FLAGS}. */
    private View flag(String name, final int bit) {
        CheckBox cb = new CheckBox(this);
        cb.setText(name);
        cb.setChecked((prefs.getInt(PREF_PARSE_FLAGS, 0) & bit) != 0);
        cb.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                int flags = prefs.getInt(PREF_PARSE_FLAGS, 0);
                prefs.edit().putInt(PREF_PARSE_FLAGS,
                        on ? (flags | bit) : (flags & ~bit)).apply();
            }
        });
        return cb;
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
