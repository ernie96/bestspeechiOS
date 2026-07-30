package com.bestspeech.tts;

import android.content.SharedPreferences;
import android.media.AudioFormat;
import android.speech.tts.SynthesisCallback;
import android.speech.tts.SynthesisRequest;
import android.speech.tts.TextToSpeech;
import android.speech.tts.TextToSpeechService;
import android.speech.tts.Voice;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Exposes BestSpeech as a system text-to-speech engine.
 *
 * <p>B32_TTS.DLL is proprietary and cannot ship inside the APK, so the user
 * supplies their own copy. Drop it at
 * {@code /Android/data/com.bestspeech.tts/files/B32_TTS.DLL} and the engine
 * picks it up on next start.
 */
public class B32TtsService extends TextToSpeechService {

    private static final String TAG = "b32tts";
    private static final String DLL_NAME = "B32_TTS.DLL";

    /** The engine is English-only; it predates any notion of locale packs. */
    private static final String LANG = "eng";
    private static final String COUNTRY = "USA";

    /**
     * How the engine's rate parameter actually maps to speed, measured on
     * "The quick brown fox jumps over the lazy dog." Speed is relative to
     * rate 0, derived from output duration.
     *
     * <p>A table rather than a formula because the curve steepens as rate rises:
     * a single exponential fit is out by nearly 2x at the slow end. The value is
     * delivered as the engine's {@code ~r} text code, since
     * bstSetParams(BST_RATE_SETTING) only affects the first utterance after the
     * engine is created -- see b32_set_rate. Both controls saturate near 2.75x,
     * which is where sonic takes over.
     */
    private static final int[] RATE_POINTS = { -200, -100, -50, -25, 0, 25, 50, 80 };
    private static final double[] SPEED_POINTS =
            { 0.362, 0.527, 0.697, 0.811, 1.000, 1.282, 1.745, 2.750 };

    private static final double SPEED_MIN = SPEED_POINTS[0];
    private static final double SPEED_MAX = SPEED_POINTS[SPEED_POINTS.length - 1];

    private long handle;
    private String currentVoice;
    private volatile boolean stopRequested;
    private long loadedStamp;

    @Override
    public void onCreate() {
        // Must run before super.onCreate(), which starts serving synthesis.
        ensureEngine();
        super.onCreate();
    }

    /**
     * Opens the engine if it is not already up.
     *
     * <p>Retried on demand rather than only at startup: the DLL is dropped in by
     * the user, so the common first-run sequence is "install, discover the file
     * is missing, copy it in". Without a retry that would need the service to be
     * force-stopped before it noticed.
     *
     * @return true if the engine is usable
     */
    private synchronized boolean ensureEngine() {
        long stamp = getSharedPreferences(B32SettingsActivity.PREFS, MODE_PRIVATE)
                .getLong(B32SettingsActivity.PREF_DLL_STAMP, 0L);
        if (handle != 0 && stamp == loadedStamp) return true;
        if (handle != 0) {
            // The settings screen imported a different engine file.
            Log.i(TAG, "engine file changed, reloading");
            B32Native.close(handle);
            handle = 0;
        }
        loadedStamp = stamp;
        try {
            byte[] dll = readDll();
            handle = B32Native.open(dll);
            currentVoice = B32Native.voiceName(0);
            Log.i(TAG, "loaded " + DLL_NAME + " (" + dll.length + " bytes), "
                    + B32Native.voiceCount() + " voices");
            return true;
        } catch (IOException e) {
            // Expected until the user supplies the file; not a stack-trace event.
            Log.w(TAG, "no engine yet (" + e.getMessage()
                    + ") -- import one from the BestSpeech settings screen");
        } catch (RuntimeException e) {
            Log.e(TAG, "engine failed to start", e);
        }
        return false;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        if (handle != 0) {
            B32Native.close(handle);
            handle = 0;
        }
    }

    private byte[] readDll() throws IOException {
        // The imported copy wins: it is what the user chose in the settings
        // screen, and it lives in private storage no other process can disturb.
        File[] candidates = {
            B32SettingsActivity.importedDll(this),
            getExternalFilesDir(null) == null ? null
                    : new File(getExternalFilesDir(null), DLL_NAME),
        };
        for (File f : candidates) {
            if (f == null || !f.isFile()) continue;
            byte[] buf = new byte[(int) f.length()];
            try (InputStream in = new FileInputStream(f)) {
                int off = 0;
                while (off < buf.length) {
                    int n = in.read(buf, off, buf.length - off);
                    if (n < 0) throw new IOException("truncated " + f);
                    off += n;
                }
            }
            return buf;
        }
        throw new IOException(DLL_NAME + " not found");
    }

    // ---------------------------------------------------------------- language

    @Override
    protected int onIsLanguageAvailable(String lang, String country, String variant) {
        if (!ensureEngine()) return TextToSpeech.LANG_MISSING_DATA;
        if (!LANG.equalsIgnoreCase(lang) && !"en".equalsIgnoreCase(lang)) {
            return TextToSpeech.LANG_NOT_SUPPORTED;
        }
        if (COUNTRY.equalsIgnoreCase(country) || "US".equalsIgnoreCase(country)) {
            return (variant == null || variant.isEmpty())
                    ? TextToSpeech.LANG_COUNTRY_AVAILABLE
                    : TextToSpeech.LANG_COUNTRY_VAR_AVAILABLE;
        }
        return TextToSpeech.LANG_AVAILABLE;
    }

    @Override
    protected String[] onGetLanguage() {
        return new String[] { LANG, COUNTRY, "" };
    }

    @Override
    protected int onLoadLanguage(String lang, String country, String variant) {
        return onIsLanguageAvailable(lang, country, variant);
    }

    @Override
    public List<Voice> onGetVoices() {
        if (!ensureEngine()) return Collections.emptyList();
        Set<String> features = new HashSet<>();
        List<Voice> out = new ArrayList<>();
        for (int i = 0; i < B32Native.voiceCount(); i++) {
            out.add(new Voice(
                    B32Native.voiceName(i),
                    Locale.forLanguageTag("en-US"),
                    Voice.QUALITY_LOW,       // 11 kHz formant synthesis, honestly labelled
                    Voice.LATENCY_VERY_LOW,  // ~200x real time
                    false,                   // no network required
                    features));
        }
        return out;
    }

    @Override
    public String onGetDefaultVoiceNameFor(String lang, String country, String variant) {
        if (!ensureEngine()) return null;
        // Must honour the settings screen: the framework fills a request's voice
        // name from here, so returning a fixed voice made the preference dead.
        return getSharedPreferences(B32SettingsActivity.PREFS, MODE_PRIVATE)
                .getString(B32SettingsActivity.PREF_VOICE, B32Native.voiceName(0));
    }

    @Override
    public int onLoadVoice(String voiceName) {
        if (!ensureEngine()) return TextToSpeech.ERROR;
        for (int i = 0; i < B32Native.voiceCount(); i++) {
            if (B32Native.voiceName(i).equals(voiceName)) {
                currentVoice = voiceName;
                return TextToSpeech.SUCCESS;
            }
        }
        return TextToSpeech.ERROR;
    }

    @Override
    public int onIsValidVoiceName(String voiceName) {
        if (!ensureEngine()) return TextToSpeech.ERROR;
        for (int i = 0; i < B32Native.voiceCount(); i++) {
            if (B32Native.voiceName(i).equals(voiceName)) return TextToSpeech.SUCCESS;
        }
        return TextToSpeech.ERROR;
    }

    // --------------------------------------------------------------- synthesis

    @Override
    protected void onStop() {
        stopRequested = true;
    }

    @Override
    protected void onSynthesizeText(SynthesisRequest request,
                                    final SynthesisCallback callback) {
        if (!ensureEngine()) {
            Log.e(TAG, "cannot synthesize: engine not loaded");
            callback.error();
            return;
        }
        stopRequested = false;

        // Read on every utterance so changes in the settings screen take effect
        // immediately rather than after the service is restarted.
        SharedPreferences prefs = getSharedPreferences(
                B32SettingsActivity.PREFS, MODE_PRIVATE);

        String voice = request.getVoiceName();
        if (voice == null || voice.isEmpty()) {
            voice = prefs.getString(B32SettingsActivity.PREF_VOICE, currentVoice);
        }
        // An unknown name would make the native side speak with no voice codes
        // at all, so fall back rather than pass it through.
        if (indexOfVoice(voice) < 0) voice = B32Native.voiceName(0);

        // Android sends rate and pitch as percentages of normal (100 = normal).
        // Every parameter is set on each utterance rather than left at whatever
        // the last one used, so output depends only on the request and the prefs.
        B32Native.setRate(handle, engineRate(request.getSpeechRate()));
        B32Native.setSpeed(handle, sonicSpeed(request.getSpeechRate()));
        B32Native.setGain(handle, clamp(
                prefs.getInt(B32SettingsActivity.PREF_GAIN,
                             B32SettingsActivity.GAIN_DEFAULT), -70, 20));
        // The request's pitch and the engine's own setting compose: 150% from the
        // caller on a voice trimmed to 80% lands at 120% of its baseline.
        B32Native.setPitch(handle, request.getPitch()
                * prefs.getInt(B32SettingsActivity.PREF_PITCH,
                               B32SettingsActivity.PITCH_DEFAULT) / 100);
        int inflection = prefs.getInt(B32SettingsActivity.PREF_INFLECTION,
                                      B32Native.VOICE_DEFAULT);
        int headSize = prefs.getInt(B32SettingsActivity.PREF_HEAD_SIZE,
                                    B32Native.VOICE_DEFAULT);
        int excitation = prefs.getInt(B32SettingsActivity.PREF_EXCITATION,
                                      B32Native.VOICE_DEFAULT);
        int unvoiced = prefs.getInt(B32SettingsActivity.PREF_UNVOICED,
                                    B32Native.VOICE_DEFAULT);
        B32Native.setVoiceParams(handle, inflection, headSize, excitation, unvoiced);

        int parseFlags = prefs.getInt(B32SettingsActivity.PREF_PARSE_FLAGS, 0);
        boolean phrase = prefs.getBoolean(
                B32SettingsActivity.PREF_PHRASE_PREDICTION, true);
        B32Native.setTextParams(handle, parseFlags, phrase);
        // One preference drives both caps: negative means leave the audio exactly
        // as the engine produced it, otherwise the trailing silence is always
        // trimmed and the number caps the pauses within the utterance.
        int pause = prefs.getInt(B32SettingsActivity.PREF_PAUSE_CAP_MS,
                                 B32SettingsActivity.PAUSE_DEFAULT);
        B32Native.setPauseCaps(handle, Math.max(pause, 0),
                pause < 0 ? 0 : B32SettingsActivity.TAIL_CAP_DEFAULT);

        String text = sanitize(request.getCharSequenceText().toString());

        Log.i(TAG, "request: rate=" + request.getSpeechRate()
                + " -> engine " + engineRate(request.getSpeechRate())
                + " x sonic " + sonicSpeed(request.getSpeechRate())
                + ", pitch=" + request.getPitch()
                + ", voice=" + voice
                + ", text=[" + text + "]");
        // Logged separately so a report of "setting X does nothing" can be
        // answered by looking at what actually reached the engine.
        Log.i(TAG, "params: inflection=" + param(inflection)
                + " head=" + param(headSize)
                + " excitation=" + param(excitation)
                + " unvoiced=" + param(unvoiced)
                + " parse=0x" + Integer.toHexString(parseFlags)
                + " phrasePrediction=" + phrase
                + " pause=" + pause);

        callback.start(B32Native.SAMPLE_RATE, AudioFormat.ENCODING_PCM_16BIT, 1);
        final int maxChunk = Math.max(512, callback.getMaxBufferSize());

        int rc = B32Native.speak(handle, text, voice, new B32Native.Sink() {
            @Override
            public boolean onAudio(byte[] pcm, int bytes) {
                if (stopRequested) return false;
                // audioAvailable() rejects writes larger than the negotiated
                // buffer size, and engine chunks are not bounded by it.
                int off = 0;
                while (off < bytes) {
                    int n = Math.min(maxChunk, bytes - off);
                    if (callback.audioAvailable(pcm, off, n) != TextToSpeech.SUCCESS) {
                        return false;
                    }
                    off += n;
                }
                return !stopRequested;
            }
        });

        if (rc != 0 && !stopRequested) {
            Log.e(TAG, "synthesis failed for voice " + voice);
            callback.error();
            return;
        }
        callback.done();
    }

    /**
     * Unicode punctuation with an unambiguous ASCII equivalent, mapped rather
     * than blanked. Otherwise iOS's curly apostrophe turns "don't" into "don t",
     * which the engine reads as two words.
     */
    private static final String CURLY =
            "‘’‚′"      // left/right/low quote, prime
            + "“”„″"    // the double-quote equivalents
            + "–—−"           // en dash, em dash, minus sign
            + "… ";                 // ellipsis, non-breaking space
    private static final String PLAIN =
            "''''" + "\"\"\"\"" + "---" + ". ";

    /**
     * Makes text safe for a 1995 ASCII engine.
     *
     * <p>Beyond the character mapping this drops {@code ~}, the engine's lead-in
     * character: left in place it is at best a homograph mark and at worst lets
     * text switch the parser into another mode for the rest of the utterance.
     */
    static String sanitize(String in) {
        StringBuilder sb = new StringBuilder(in.length());
        for (int i = 0; i < in.length(); i++) {
            char c = in.charAt(i);
            int k = CURLY.indexOf(c);
            if (k >= 0) c = PLAIN.charAt(k);
            if (c == '~') continue;
            sb.append(c >= 0x20 && c <= 0x7E ? c : ' ');
        }
        return sb.toString();
    }

    private int indexOfVoice(String name) {
        if (name == null) return -1;
        for (int i = 0; i < B32Native.voiceCount(); i++) {
            if (B32Native.voiceName(i).equals(name)) return i;
        }
        return -1;
    }

    private static String param(int v) {
        return v == B32Native.VOICE_DEFAULT ? "voice" : Integer.toString(v);
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /**
     * The engine rate parameter that gets closest to the requested speed,
     * interpolated within {@link #RATE_POINTS} and clamped to what it can do.
     */
    static int engineRate(int androidPercent) {
        double want = androidPercent / 100.0;
        if (want <= SPEED_MIN) return RATE_POINTS[0];
        int last = RATE_POINTS.length - 1;
        if (want >= SPEED_MAX) return RATE_POINTS[last];
        for (int i = 0; i < last; i++) {
            if (want <= SPEED_POINTS[i + 1]) {
                // Interpolate in log-speed: the parameter behaves multiplicatively.
                double t = (Math.log(want) - Math.log(SPEED_POINTS[i]))
                        / (Math.log(SPEED_POINTS[i + 1]) - Math.log(SPEED_POINTS[i]));
                return (int) Math.round(
                        RATE_POINTS[i] + t * (RATE_POINTS[i + 1] - RATE_POINTS[i]));
            }
        }
        return 0;
    }

    /**
     * The leftover factor sonic has to supply, once the engine has been pushed as
     * far as it goes. 1.0 whenever the request is within the engine's own range,
     * so ordinary speeds never pay for time-stretching.
     *
     * <p>This is what makes the 400-500% settings screen-reader users prefer
     * actually work: the engine tops out at ~2.75x and sonic covers the rest,
     * preserving pitch.
     */
    static float sonicSpeed(int androidPercent) {
        double want = androidPercent / 100.0;
        if (want > SPEED_MAX) return (float) (want / SPEED_MAX);
        if (want < SPEED_MIN) return (float) (want / SPEED_MIN);
        return 1.0f;
    }
}
